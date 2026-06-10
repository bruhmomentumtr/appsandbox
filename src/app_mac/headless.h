/*
 * headless.h -- AppSandbox headless daemon (AppSandbox --headless), macOS.
 * Mirrors src/app_win/headless.h.
 */

#ifndef HEADLESS_MAC_H
#define HEADLESS_MAC_H

/* Run the AppSandbox headless daemon. Owns the core for the process lifetime
   and serves a Docker-style local HTTP API over loopback. Returns the process
   exit code (2 = another AppSandbox instance is running). */
int run_headless(int argc, const char *argv[]);

/* Acquire the cross-mode single-instance lock: flock(LOCK_EX|LOCK_NB) on
   ~/Library/Application Support/AppSandbox/instance.lock. GUI and daemon take
   the SAME lock, so they are mutually exclusive (both own the one vms.cfg,
   which has no file locking). Returns the held fd -- keep it open for the
   process lifetime (the OS releases it on any exit, including crashes) -- or
   -1 if another AppSandbox holds it. */
int asb_instance_lock_acquire(void);

#endif /* HEADLESS_MAC_H */
