/*
 * appsandbox-audio.c — Linux audio capture for AppSandbox.
 *
 * Listens on AF_VSOCK port 4, captures the system audio output from the
 * snd-aloop kernel loopback device, and ships PCM frames over vsock in
 * the ASA1 wire format the Windows host already understands (see
 * src/backend_win/vm_display_idd.c).
 *
 * Architecture — kernel-level capture, no PipeWire/PulseAudio in the
 * hot path:
 *
 *   app → pipewire (or any audio API)
 *           ↓ ALSA backend
 *         hw:Loopback,0 (playback)
 *           ↓ kernel snd-aloop module loops here
 *         hw:Loopback,1 (capture) ← we read this with libasound
 *           ↓
 *         vsock :4 → host WASAPI render
 *
 * The user-session audio routing (telling PipeWire to use hw:Loopback,0
 * as the default sink) is set up separately via a wireplumber policy
 * file; this daemon assumes it's already in place.
 *
 * Wire protocol (see docs/linux-idd-implementation-plan.md §"Port 4"):
 *
 *   one-shot AudioHeader on connect:
 *     u32 magic        = 'ASA1' (0x31415341)
 *     u32 sample_rate  = 48000
 *     u16 channels     = 2
 *     u16 bits         = 32
 *     u16 format_tag   = 1 (WAVE_FORMAT_PCM)
 *     u16 block_align  = 8
 *
 *   then repeating:
 *     u32 bytes
 *     u8  pcm[bytes]
 *
 * Build:  gcc -O2 -Wall -o appsandbox-audio appsandbox-audio.c -lasound
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>

#include <alsa/asoundlib.h>

#define VSOCK_PORT          4
#define AUDIO_HEADER_MAGIC  0x31415341u   /* 'ASA1' */

/* Negotiated format. snd-aloop requires the capture and playback halves
 * to have matching hw_params — we can't negotiate independently. PipeWire
 * opens its end as S32_LE / 48k / 2ch / 1024-frame period, so we follow
 * suit. WASAPI on the host side accepts S32 PCM via format_tag=1
 * (WAVE_FORMAT_PCM) and bits_per_sample=32. */
#define SAMPLE_RATE         48000
#define CHANNELS            2
#define BITS_PER_SAMPLE     32
#define BLOCK_ALIGN         ((CHANNELS * BITS_PER_SAMPLE) / 8)   /* 8 */

/* PipeWire's ALSA backend opens snd-aloop with a 1024-frame period at
 * 48 kHz = ~21 ms. Match it so the capture side's hw_params line up. */
#define FRAMES_PER_PACKET   1024
#define BYTES_PER_PACKET    (FRAMES_PER_PACKET * BLOCK_ALIGN)

/* snd-aloop pairs playback subdev N on device 0 with capture subdev N
 * on device 1. We capture subdev 0 (hw:Loopback,1,0); the WirePlumber
 * policy routes desktop playback to the matching hw:Loopback,0,0. */
#define LOOPBACK_CAPTURE_PCM   "hw:Loopback,1,0"

#pragma pack(push, 1)
struct audio_header {
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t format_tag;
    uint16_t block_align;
};
struct audio_frame_header {
    uint32_t bytes;
};
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void aud_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[audio] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- vsock listener ---- */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { aud_log("socket: %s", strerror(errno)); return -1; }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        aud_log("bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        aud_log("listen: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* ---- ALSA capture from snd-aloop ---- */

static snd_pcm_t *open_loopback_capture(void)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw = NULL;
    int rc;

    rc = snd_pcm_open(&pcm, LOOPBACK_CAPTURE_PCM, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        aud_log("snd_pcm_open %s: %s", LOOPBACK_CAPTURE_PCM, snd_strerror(rc));
        return NULL;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    /* MUST be non-interleaved: snd-aloop's loopback_check_format() in
     * sound/drivers/aloop.c returns -EIO when capture and playback
     * disagree on is_access_interleaved(). PipeWire opens its end as
     * MMAP_NONINTERLEAVED, so we follow suit. We interleave in user
     * space before shipping over the wire because the host's WASAPI
     * IAudioClient expects interleaved L/R samples. */
    snd_pcm_hw_params_set_access (pcm, hw, SND_PCM_ACCESS_RW_NONINTERLEAVED);
    snd_pcm_hw_params_set_format (pcm, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
    rc = snd_pcm_hw_params_set_rate(pcm, hw, SAMPLE_RATE, 0);
    if (rc < 0) aud_log("set_rate strict failed: %s — retrying _near", snd_strerror(rc));
    if (rc < 0) {
        unsigned rate = SAMPLE_RATE;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    }
    /* snd-aloop requires the period AND buffer size to match the
     * playback half. PipeWire opens its end as 1024-frame period,
     * 32768-frame buffer; we mirror exactly. Without strict matching,
     * snd_pcm_readi returns -EIO regardless of how active the playback
     * side is. */
    rc = snd_pcm_hw_params_set_period_size(pcm, hw, FRAMES_PER_PACKET, 0);
    if (rc < 0) {
        aud_log("set_period_size strict failed: %s", snd_strerror(rc));
        snd_pcm_uframes_t period = FRAMES_PER_PACKET;
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    }
    rc = snd_pcm_hw_params_set_buffer_size(pcm, hw, FRAMES_PER_PACKET * 32);
    if (rc < 0) {
        aud_log("set_buffer_size strict failed: %s", snd_strerror(rc));
        snd_pcm_uframes_t buffer = FRAMES_PER_PACKET * 32;
        snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    }

    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        aud_log("snd_pcm_hw_params: %s", snd_strerror(rc));
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_prepare(pcm);
    {
        snd_pcm_uframes_t pp = 0, bp = 0;
        unsigned actual_rate = 0;
        int dir = 0;
        snd_pcm_hw_params_get_rate       (hw, &actual_rate, &dir);
        snd_pcm_hw_params_get_period_size(hw, &pp, &dir);
        snd_pcm_hw_params_get_buffer_size(hw, &bp);
        aud_log("capturing from %s @ %u Hz, %u ch, S32_LE, period=%lu, buffer=%lu",
                LOOPBACK_CAPTURE_PCM, actual_rate, CHANNELS,
                (unsigned long)pp, (unsigned long)bp);
    }
    return pcm;
}

/* ---- session: handshake + stream loop ---- */

static void serve(int client_fd)
{
    snd_pcm_t *pcm = open_loopback_capture();
    if (!pcm) return;

    struct audio_header h = {
        .magic           = AUDIO_HEADER_MAGIC,
        .sample_rate     = SAMPLE_RATE,
        .channels        = CHANNELS,
        .bits_per_sample = BITS_PER_SAMPLE,
        .format_tag      = 1,                   /* WAVE_FORMAT_PCM */
        .block_align     = BLOCK_ALIGN,
    };
    if (send_all(client_fd, &h, sizeof(h)) < 0) {
        aud_log("send header: %s", strerror(errno));
        snd_pcm_close(pcm);
        return;
    }

    /* Stream loop. snd_pcm_readn blocks until a period's worth of frames
     * is available — the snd-aloop kernel module fills its ring as soon
     * as the playback side writes, so this also paces our send rate.
     *
     * When nothing is playing into the loopback, snd-aloop returns -EIO
     * (no producer). Recover and keep the connection open by sending
     * silence at the same cadence — keeps the host's WASAPI render alive
     * and avoids the connect/disconnect churn the host would otherwise
     * see while the desktop is idle. */
    /* Non-interleaved capture: snd-aloop requires it (see access mode
     * comment in open_loopback_capture). Read into per-channel arrays
     * and interleave into `interleaved[]` for the wire send. */
    int32_t left[FRAMES_PER_PACKET];
    int32_t right[FRAMES_PER_PACKET];
    void *channel_bufs[CHANNELS] = { left, right };
    uint8_t interleaved[BYTES_PER_PACKET];
    uint8_t silence[BYTES_PER_PACKET] = {0};
    int eio_warned = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    /* Pace at the packet's true duration. The hardcoded 10 ms was shorter than
       the ~21.3 ms a FRAMES_PER_PACKET packet represents, so idle silence was
       sent at ~2x realtime and overflowed the host render ring (L13). */
    const long packet_ns = (long)FRAMES_PER_PACKET * 1000000000L / SAMPLE_RATE; /* ~21.33 ms */

    while (!g_stop) {
        snd_pcm_sframes_t n = snd_pcm_readn(pcm, channel_bufs, FRAMES_PER_PACKET);
        if (n == -EPIPE || n == -EIO) {
            if (!eio_warned) {
                aud_log("readi err %s (%ld) — shipping silence",
                        snd_strerror((int)n), (long)n);
                eio_warned = 1;
            }
            snd_pcm_prepare(pcm);
            struct audio_frame_header fh = { .bytes = BYTES_PER_PACKET };
            if (send_all(client_fd, &fh, sizeof(fh)) < 0) break;
            if (send_all(client_fd, silence, BYTES_PER_PACKET) < 0) break;
            /* Pace at one packet per period. */
            next.tv_nsec += packet_ns;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L; next.tv_sec += 1;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            continue;
        }
        if (n < 0) {
            aud_log("snd_pcm_readi: %s", snd_strerror((int)n));
            break;
        }
        if (eio_warned) { aud_log("loopback live again"); eio_warned = 0; }
        /* Interleave L/R int32 samples into the send buffer. */
        for (snd_pcm_sframes_t i = 0; i < n; i++) {
            ((int32_t *)interleaved)[i * 2 + 0] = left[i];
            ((int32_t *)interleaved)[i * 2 + 1] = right[i];
        }
        size_t bytes = (size_t)n * BLOCK_ALIGN;
        struct audio_frame_header fh = { .bytes = (uint32_t)bytes };
        if (send_all(client_fd, &fh, sizeof(fh)) < 0) break;
        if (send_all(client_fd, interleaved, bytes) < 0) break;
        clock_gettime(CLOCK_MONOTONIC, &next);   /* resync timer */
    }

    aud_log("session ended");
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    /* Install SIGINT/SIGTERM WITHOUT SA_RESTART so a blocking accept()
     * returns EINTR on signal (the loop below checks for it) instead of
     * auto-restarting. glibc's signal() defaults to BSD SA_RESTART
     * semantics, which would leave accept() blocked through shutdown and
     * make systemd wait the full stop timeout. Mirrors the agent. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    int srv = vsock_listen(VSOCK_PORT);
    if (srv < 0) return 1;
    aud_log("listening on vsock :%u", VSOCK_PORT);

    while (!g_stop) {
        struct sockaddr_vm peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            aud_log("accept: %s", strerror(errno));
            break;
        }
        aud_log("client connected (cid=%u)", peer.svm_cid);
        serve(c);
        close(c);
    }

    close(srv);
    return 0;
}
