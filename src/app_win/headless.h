#ifndef HEADLESS_H
#define HEADLESS_H

#include <windows.h>

/* Run the AppSandbox headless daemon (appsandbox.exe --headless).
   Owns the core for the process lifetime and serves a local HTTP API.
   Returns the process exit code. */
int run_headless(HINSTANCE hInst, const wchar_t *cmdline);

#endif /* HEADLESS_H */
