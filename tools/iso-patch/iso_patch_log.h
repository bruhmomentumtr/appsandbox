#ifndef ISO_PATCH_LOG_H
#define ISO_PATCH_LOG_H

#include <wchar.h>

void log_msg(const wchar_t *fmt, ...);
void log_progress(int pct, const wchar_t *step);
void log_done(const wchar_t *path);
void log_err(const wchar_t *fmt, ...);

#endif
