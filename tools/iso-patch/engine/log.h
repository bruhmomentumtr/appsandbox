/* log.h - timestamped, two-sink (stdout + file) logger.
 *
 * Every log call emits a line of the form:
 *
 *   [HH:MM:SS.mmm] [PHASE] [LEVEL] message
 *
 * to BOTH stdout (so you see it live) AND the log file (so you can paste
 * it back). The point of this POC is to make failures debuggable from
 * the log alone - so log liberally, log every Win32 call's parameters
 * AND return value, log every computed offset/size/UUID.
 *
 * LOG_HEX dumps a buffer as hex+ASCII (16 bytes per line) - useful for
 * eyeballing partition headers, superblocks, etc.
 *
 * LOG_WIN32_FAIL pulls FormatMessage on GetLastError and includes it.
 */
#ifndef POC_LOG_H
#define POC_LOG_H

#include <windows.h>
#include <stdio.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/* Open the log file. path may be NULL -> auto-name out\pocbuild-<ts>.log.
 * Sets the current phase to "INIT". Returns 0 on success. */
int  log_open(const char *path);
void log_close(void);

/* Tag emitted in [PHASE] for subsequent log lines. Pushed/popped at
 * phase boundaries (Phase 0, Phase 1, ...). Max 8 stack depth. */
void log_phase_push(const char *phase);
void log_phase_pop(void);

/* Core log. Use the LOG_* macros below for source-line context. */
void log_emit(log_level_t lvl, const char *file, int line,
              const char *fmt, ...);

/* Hex dump - prints up to `len` bytes from `buf` in 16-byte rows. */
void log_hex(log_level_t lvl, const char *label, const void *buf, size_t len);

/* Convenience for failed Win32 calls. Logs the call name, the error
 * code, and FormatMessage's English text. */
void log_win32_fail(const char *file, int line, const char *call,
                    DWORD err);

#define LOG_D(...)  log_emit(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_I(...)  log_emit(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_W(...)  log_emit(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_E(...)  log_emit(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_HEX(lvl, label, buf, len)  log_hex((lvl), (label), (buf), (len))
#define LOG_WIN32(call, err)  log_win32_fail(__FILE__, __LINE__, (call), (err))

#endif
