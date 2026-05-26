/* log.c - implementation of the two-sink logger. */
#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>

#define MAX_PHASE_DEPTH 8

static FILE *g_log_file = NULL;
static const char *g_phase_stack[MAX_PHASE_DEPTH] = { "INIT" };
static int g_phase_depth = 1;

static const char *level_str(log_level_t lvl)
{
    switch (lvl) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

static void emit_line_prefix(FILE *fp, log_level_t lvl,
                             const char *file, int line)
{
    struct _timeb tb;
    _ftime64_s(&tb);
    struct tm lt;
    localtime_s(&lt, &tb.time);

    const char *base = file;
    for (const char *p = file; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }

    fprintf(fp, "[%02d:%02d:%02d.%03u] [%s] [%s] %s:%d ",
            lt.tm_hour, lt.tm_min, lt.tm_sec, tb.millitm,
            g_phase_stack[g_phase_depth - 1],
            level_str(lvl),
            base, line);
}

int log_open(const char *path)
{
    char auto_path[MAX_PATH];

    if (path == NULL) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_s(&lt, &now);
        CreateDirectoryA("out", NULL);  /* ok if exists */
        snprintf(auto_path, sizeof(auto_path),
                 "out\\pocbuild-%04d%02d%02d-%02d%02d%02d.log",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
        path = auto_path;
    }

    g_log_file = fopen(path, "wb");
    if (!g_log_file) {
        fprintf(stderr, "log_open: cannot open %s (errno=%d)\n",
                path, errno);
        return -1;
    }

    /* No buffering - we want every line on disk in case we crash. */
    setvbuf(g_log_file, NULL, _IONBF, 0);

    LOG_I("===== pocbuild log opened: %s =====", path);
    return 0;
}

void log_close(void)
{
    if (g_log_file) {
        LOG_I("===== log closed =====");
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_phase_push(const char *phase)
{
    if (g_phase_depth < MAX_PHASE_DEPTH) {
        g_phase_stack[g_phase_depth++] = phase;
        LOG_I(">>> ENTER phase %s", phase);
    }
}

void log_phase_pop(void)
{
    if (g_phase_depth > 1) {
        const char *p = g_phase_stack[--g_phase_depth];
        LOG_I("<<< EXIT  phase %s", p);
    }
}

void log_emit(log_level_t lvl, const char *file, int line,
              const char *fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    emit_line_prefix(stdout, lvl, file, line);
    fputs(msg, stdout);
    fputc('\n', stdout);
    fflush(stdout);

    if (g_log_file) {
        emit_line_prefix(g_log_file, lvl, file, line);
        fputs(msg, g_log_file);
        fputc('\n', g_log_file);
    }
}

void log_hex(log_level_t lvl, const char *label, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    char line[128];

    log_emit(lvl, __FILE__, __LINE__, "HEX [%s] %zu bytes:", label, len);

    for (size_t off = 0; off < len; off += 16) {
        int n = snprintf(line, sizeof(line), "  %08zx  ", off);
        size_t row = (len - off < 16) ? (len - off) : 16;

        for (size_t i = 0; i < 16; i++) {
            if (i < row) {
                n += snprintf(line + n, sizeof(line) - n, "%02x ", p[off + i]);
            } else {
                n += snprintf(line + n, sizeof(line) - n, "   ");
            }
            if (i == 7) {
                n += snprintf(line + n, sizeof(line) - n, " ");
            }
        }
        n += snprintf(line + n, sizeof(line) - n, " |");
        for (size_t i = 0; i < row; i++) {
            unsigned char c = p[off + i];
            n += snprintf(line + n, sizeof(line) - n, "%c",
                          (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        snprintf(line + n, sizeof(line) - n, "|");

        log_emit(lvl, __FILE__, __LINE__, "%s", line);
    }
}

void log_win32_fail(const char *file, int line, const char *call, DWORD err)
{
    char buf[512] = { 0 };
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, sizeof(buf) - 1, NULL);

    /* Strip trailing whitespace - FormatMessage appends CRLF. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
        buf[--n] = 0;
    }

    log_emit(LOG_ERROR, file, line,
             "WIN32 FAIL: %s err=0x%08lx (%lu) - %s",
             call, err, err, buf);
}
