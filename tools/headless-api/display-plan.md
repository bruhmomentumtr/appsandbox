# Headless display-open plan (Windows + macOS)

Expose "open the VM's display window" through the headless CLI/SDK, so a script can
do `c.open_display("vm")` / `c.close_display("vm")` the way the GUI's Connect button
does. The user can also close the window with its **X button**, and the daemon must
notice and reflect that.

> **STATUS**
> - **Windows: implementable and testable now.** This is the target of the first
>   pass.
> - **macOS: design only — UNVALIDATED.** We have no Mac to build/run it on. The
>   macOS section is a worked design plus the one spike that must pass on real
>   hardware before it can be trusted (it swaps the daemon's run loop). Do not ship
>   the macOS half on the strength of this document alone.

---

## 1. How the display actually works (so the plan is grounded)

**Windows.** The guest's IDDCX driver (`tools/vdd/vdd.cpp`) creates a virtual
monitor; Windows renders the desktop into the IDDCX swap chain, giving the driver
RGB/BGRA buffers. The driver is the **listener**: `socket(AF_HYPERV)` →
`bind`/`listen(…,1)`/`accept` on the frame service GUID `…0002…`
(`vdd.cpp:166-220`). The **host** is the connector: `connect_to_hv_service(&vm->runtime_id,
FRAME_SERVICE_GUID, …)` (`vm_display_idd.c:763`) opens an `AF_HYPERV` socket to that
vsock using the VM's `runtime_id`, and `idd_recv_thread_proc` reads frames and
renders them into a D3D11 window. **The host opening that connection is the act that
makes frames flow** ("direct HvSocket frame transport to the host", `vdd.cpp:10`).
`vm_display_idd_create(vm, HINSTANCE, HWND main_hwnd)` (`vm_display_idd.h:13`) is
**self-contained**: it spawns its own window thread (`:2393`) with its own message
pump (`:1994`) and D3D11 renderer; `main_hwnd` is used *only* to `PostMessage`
`WM_VM_DISPLAY_CLOSED` on close (`:2129`).

**macOS.** The display is a `VZVirtualMachineView` hosted in an `NSWindow`
(`VzDisplayWindow`, `vz_display.m`). The core creates it on the VM's **Running**
transition, gated by `g_headless` (`asb_core_mac.m:586-590`), tracks it in
`g_display_refs[]`, and closes it on stop/delete (`:567-571`, `:1070-1086`).
Crucially, the **`VZVirtualMachine` is in-process** (`vz_handle` points at the
in-process `VzVm`), so the view can only be created **inside the daemon process** —
it cannot be offloaded to a helper GUI app, unlike Windows where frames arrive over a
socket any process could render.

---

## 2. Access model (what gates the feature)

Loopback is **not** an identity or visibility boundary — any local process (any user,
any session, SSH) can connect to `127.0.0.1`, and there is no robust way to identify
the peer of a loopback TCP connection (macOS has no TCP peer-PID at all;
`LOCAL_PEERPID` is Unix-domain-socket only; Windows' `GetExtendedTcpTable` mapping is
a racy snapshot). So we do **not** try to sniff the caller. The real gates:

- **Authorization — who may ask the daemon:** the existing **bearer token** in
  `host.json` (gated by that file's ACL). Unchanged. Not loopback.
- **Frames (B) — who can read the VM's pixels:** the `AF_HYPERV` frame connect needs
  the VM's `runtime_id` + HvSocket access to that partition. The **daemon has both
  because it owns the compute system**; clients never touch the vsock — they go
  through the daemon. So B is satisfied *structurally*; we never enforce it against
  clients. (Whether the OS hard-ACLs the partition endpoint vs. gating it by "you
  need the runtime_id" is a threat-model footnote on a single-admin box, not a design
  input.)
- **Window (A) — can a window actually be shown:** the only runtime gate. A property
  of the **daemon's own session**, checked directly — no caller introspection:
  - Windows: visible window station — `GetProcessWindowStation()` +
    `GetUserObjectInformation(UOI_FLAGS)` → `WSF_VISIBLE` (interactive `WinSta0`, not
    the non-visible service station).
  - macOS: `CGSessionCopyCurrentDictionary()` non-null with `kCGSessionOnConsoleKey`
    true (only inside a console Aqua login session).
  - If A is false → reject with a clear "no local display available" error instead of
    spawning an invisible window.

**Why this is right for the normal case:** the user launches the daemon in their
interactive desktop session and drives it via the loopback script (with the token).
The daemon, being in that same session, satisfies A, so the window opens on the user's
desktop. No detection, no rejection — it just works. A only bites when the daemon has
no desktop (service session / headless SSH), which is exactly when it *should* refuse.

---

## 3. Shared API surface

- `POST   /v1/vms/{name}/display` → open (or focus if already open). `409` if the VM
  isn't running; `409 no_display` if A is false.
- `DELETE /v1/vms/{name}/display` → close.
- `GET    /v1/vms/{name}` → add `"displayOpen": true|false`.
- SDK (`asb.py`):
  ```python
  def open_display(self, name):  return self._req("POST",   "/vms/%s/display" % name)
  def close_display(self, name): return self._req("DELETE", "/vms/%s/display" % name)
  # status() already returns the dict; displayOpen rides along
  ```
  → `c.open_display("vm")` is the `idd.connect("vm")` shape.

Put the **A-gate in the shared endpoint layer** so both platforms enforce it before
touching any window code, behind a small `asb_host_can_show_window()` helper.

---

## 4. Windows implementation (build this first)

`vm_display_idd_create` is self-contained, and the daemon already has what it needs:
the `HINSTANCE` (`run_headless` → `asb_set_hinstance`, `headless.c:1070`) and
`asb_vm_instance(vm)` to get the `VmInstance*`. The GUI keeps its display table in
`ui.c` (`g_idd_displays[]`); move that into the core (or keep a daemon-side table in
`headless.c`) so the daemon owns one.

```c
/* headless.c */
static VmDisplayIdd *g_displays[ASB_MAX_VMS];   /* per-VM, daemon-owned */

/* POST /v1/vms/{name}/display */
if (!host_window_station_visible())              /* A-gate (WSF_VISIBLE) */
    -> 409 {"error":"no_display"};
VmInstance *v = asb_vm_instance(vm);
if (!v || !v->running) -> 409;
int idx = vm_index;
if (g_displays[idx] && vm_display_idd_is_open(g_displays[idx]))
    vm_display_idd_focus(g_displays[idx]);       /* already open -> focus */
else
    g_displays[idx] = vm_display_idd_create(v, g_hinst, /*main_hwnd*/NULL);

/* DELETE /v1/vms/{name}/display */
vm_display_idd_destroy(g_displays[idx]);  g_displays[idx] = NULL;
```

**X-button close.** Today the display posts `WM_VM_DISPLAY_CLOSED` to `main_hwnd`
(`vm_display_idd.c:2129`); with `main_hwnd = NULL` that notification is lost, and the
daemon's request loop is single-threaded (`headless.c:12`) so it has no message pump
to receive it anyway. Fix: add a **close callback** to `vm_display_idd` — a function
pointer (+ user data) the creator registers, invoked from the display's own window
thread on `WM_CLOSE`/`WM_DESTROY`:
```c
/* vm_display_idd.h */
typedef void (*VmDisplayIddOnClose)(void *ud);
VmDisplayIdd *vm_display_idd_create_cb(VmInstance *vm, HINSTANCE hInstance,
                                       VmDisplayIddOnClose on_close, void *ud);
/* headless.c: on_close nils g_displays[idx] so displayOpen flips false. */
```
The GUI can keep its `main_hwnd`/`WM_VM_DISPLAY_CLOSED` path or migrate to the
callback; either way no message pump is needed in the daemon.

**Cleanup.** On daemon exit, destroy any open `g_displays[]`. VM stop/delete already
tears displays down in the GUI path (`safe_destroy_idd`); ensure the daemon table is
nilled on the stop/delete handlers too so `displayOpen` stays honest.

**A-gate helper (Windows):**
```c
static BOOL host_window_station_visible(void) {
    HWINSTA ws = GetProcessWindowStation();
    USEROBJECTFLAGS f = {0}; DWORD need = 0;
    if (ws && GetUserObjectInformationW(ws, UOI_FLAGS, &f, sizeof(f), &need))
        return (f.dwFlags & WSF_VISIBLE) != 0;
    return FALSE;
}
```

**Caveat (document, don't fix):** the window opens on the **daemon's session
desktop** — visible when launched from the interactive elevated shell (today's model),
invisible if ever run as a session-0 service. The A-gate refuses the latter.

**Effort:** ~self-contained: core open/close/query + the close-callback + endpoint +
2 SDK methods. **No agent change, no IDDCX change.**

---

## 5. macOS design (DESIGN ONLY — cannot validate here)

Because the `VZVirtualMachine` is in-process, the window must be created **in the
daemon**, so the daemon must be GUI-capable, so it must run **inside a GUI login
session**. That constraint is immovable (OS reality), but the code delta is contained.

**A-gate helper (macOS):**
```objc
#import <CoreGraphics/CoreGraphics.h>
BOOL asb_mac_have_gui_session(void) {
    CFDictionaryRef info = CGSessionCopyCurrentDictionary();
    if (!info) return NO;
    CFBooleanRef on = CFDictionaryGetValue(info, kCGSessionOnConsoleKey);
    BOOL ok = (on && CFBooleanGetValue(on));
    CFRelease(info);
    return ok;
}
```

**Run-loop branch (`headless.m`)** — the load-bearing change. The headless daemon
currently runs a bare `CFRunLoopRun()` with no `NSApplication` (`headless.m:1110`,
deliberately, so it works over SSH/launchd). Replace it with a startup branch:
```objc
#import <AppKit/AppKit.h>
...
/* The VZVirtualMachine is in-process, so a display window must be created here.
   In a GUI session, run the AppKit loop -- [NSApp run] drains the main dispatch
   queue exactly like CFRunLoopRun, so every main-queue VZ op is unaffected -- and
   windows can open on demand. Otherwise keep the bare CFRunLoop: headless, no
   window (SSH / launchd), unchanged. */
if (asb_mac_have_gui_session()) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp run];
} else {
    CFRunLoopRun();
}
```
The quit path must match the loop: in the NSApp branch, replace
`CFRunLoopStop(CFRunLoopGetMain())` with `[NSApp stop:nil]` + a dummy event to wake
it (or `[NSApp terminate:nil]`).

**Open / close / query (`asb_core_mac.m`)** — reuses the exact window code the
Running-transition path already runs (`:586-590`), bypassing `g_headless`, on the
main queue (HTTP runs on a worker thread, so `dispatch_sync` to main is safe):
```objc
int asb_mac_open_display(const char *name) {
    if (!asb_mac_have_gui_session()) return BACKEND_NO_DISPLAY;      /* A-gate */
    __block int rc = BACKEND_OK;
    dispatch_sync(dispatch_get_main_queue(), ^{
        int idx = vm_index_of(name);
        if (idx < 0)                                      { rc = BACKEND_NOT_FOUND;   return; }
        if (!g_vms[idx].running || !g_vms[idx].vz_handle) { rc = BACKEND_NOT_RUNNING; return; }
        if (g_display_refs[idx]) {                                   /* already open -> focus */
            [[(VzDisplayWindow *)g_display_refs[idx] window] makeKeyAndOrderFront:nil];
            return;
        }
        VzDisplayWindow *d = [[VzDisplayWindow alloc] initWithVzVm:g_vms[idx].vz_handle];
        g_display_refs[idx] = d; g_vms[idx].display = d; [d showDisplay];
    });
    return rc;
}
int asb_mac_close_display(const char *name) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        int idx = vm_index_of(name);
        if (idx < 0 || !g_display_refs[idx]) return;
        [[(VzDisplayWindow *)g_display_refs[idx] window] close];     /* fires windowWillClose: */
        g_display_refs[idx] = nil; g_vms[idx].display = nil;
    });
    return BACKEND_OK;
}
BOOL asb_mac_display_open(const char *name) {
    int idx = vm_index_of(name);
    return (idx >= 0 && g_display_refs[idx] != nil);
}
void asb_mac_display_closed(const char *name) {   /* user hit X; called on main */
    int idx = vm_index_of(name);
    if (idx < 0) return;
    g_display_refs[idx] = nil; g_vms[idx].display = nil;
}
```

**X-button close (`vz_display.m`)** — one line added to the existing
`windowWillClose:` (`:49`):
```objc
- (void)windowWillClose:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vm.name.UTF8String, YES);
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, NO);
    asb_mac_display_closed(self.vm.name.UTF8String);   /* clear core ref */
}
```

**`sudo` nuance (don't reason on UID; the A-gate decides at runtime):** macOS gates the
GUI on the bootstrap namespace / Aqua session, not the user id. `sudo ./daemon` from a
Terminal *in the active GUI session* inherits that session's namespace and keeps
window-server access (root or not); a `launchd` **system daemon** runs in the root
session with no GUI; SSH is unreliable. `CGSessionCopyCurrentDictionary()` reflects the
truth in all three cases, so the same A-gate handles them without any sudo-specific
logic.

**The spike that must pass on real hardware before trusting any of this:** stand up
`[NSApplication sharedApplication]` + `[NSApp run]` in this daemon and confirm
(a) the existing main-queue VZ dispatches still fire (start/stop/state), and (b) a
`VzDisplayWindow` opens, accepts keyboard/mouse, and the X-button delivers
`windowWillClose:`. ~90% confidence on (a) — `[NSApp run]` and `CFRunLoopRun` both
drain the main run loop / main queue — but it is load-bearing and we cannot run it
here. **Until that spike passes on a Mac, the macOS half is not done.**

---

## 6. Edge cases (both platforms)

- VM not running → `409`. Open when already open → focus + `200`. Close when already
  closed → no-op.
- VM stops / is deleted while the display is open → existing stop/delete paths close
  the window; ensure the daemon's table / `g_display_refs` is nilled so `displayOpen`
  tracks it.
- Daemon exit → close all open displays.
- Single consumer: the IDDCX driver `listen(…,1)` accepts one host connection, so at
  most one display per VM — which the per-VM table already enforces.

---

## 7. Rollout

1. **Finish this plan** (this document).
2. **Commit current branch state** — the fire-and-forget graceful-shutdown change
   (`asb_core.c` / `vm_agent.c` / `vm_agent.h` / `vm_display_idd.c`) is uncommitted;
   reconcile against `origin/headless-api` and commit if changed.
3. **Implement the Windows half** (section 4): build, then validate with the test
   suite + a manual `c.open_display` from an interactive session (window appears,
   X-close flips `displayOpen`, daemon stays responsive — single-threaded request
   loop must not be blocked by the display, which the self-contained window thread
   guarantees).
4. **macOS half is deferred** — landed as code behind the A-gate, but marked
   unvalidated until the section-5 spike runs on a Mac. The A-gate means a macOS
   daemon without a GUI session simply refuses `open_display`, so shipping the code
   inert is safe; enabling/trusting it requires the spike.

---

## 8. References (research behind the access model)

- Peer-credentials need a Unix-domain socket / named pipe, not loopback TCP:
  golang/go #27613 (`LOCAL_PEERPID` is UDS-only on darwin); SO_PEERCRED notes
  (UDS-only); Microsoft `GetExtendedTcpTable` (snapshot, racy).
- macOS GUI access is bootstrap-namespace / Aqua-session scoped, not UID-scoped:
  "Accessing the macOS GUI in Automation Contexts" (aahlenst.dev); Apple "Root and
  Login Sessions"; Apple `CGSessionCopyCurrentDictionary()`; `pam_reattach`.
