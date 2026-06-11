# Headless display-open plan (Windows + Linux + macOS)

Expose "open the VM's display window" through the headless CLI/SDK, so a script can
do `c.open_display("vm")` / `c.close_display("vm")` the way the GUI's Connect button
does. The user can also close the window with its **X button**, and the daemon
reflects that.

> **STATUS**
> - **Windows: implemented and validated.** Open/close/poll endpoints, the latched
>   readiness flag, and the per-VM display table are in `headless.c` + `asb_core.c`;
>   the SDK has `open_display`/`close_display`/`display_status`/`display_ready`.
>   Validated by hand: two VMs opened the instant readiness latched, close + reopen,
>   then graceful shutdown + remove — the daemon stayed alive through every path.
> - **Linux: implemented and validated.** Same host-side machinery; the readiness
>   signal is the Linux guest agent reporting `idd_status:ok` once the user session is
>   up (Mutter compositing). Validated on fresh Linux VMs built from the pushed
>   `headless-api` branch — both latched ready via `idd_status` and opened their
>   displays, daemon stable.
> - **macOS: design only — UNVALIDATED.** We have no Mac to build/run it on. The
>   macOS section is a worked design plus the one spike that must pass on real
>   hardware (it swaps the daemon's run loop). Do not ship the macOS half on the
>   strength of this document alone.

---

## 1. How the display works (so the plan is grounded)

The single fact that shapes everything below: **the frame channel is the *consumer*
channel — connecting to it *is* becoming the display.** So readiness can never be
discovered by "testing" it; readiness comes from the guest agent instead.

**Windows.** The guest's IDDCX driver (`tools/vdd/vdd.cpp`) creates a virtual monitor;
Windows composites the desktop into the IDDCX swap chain. The driver's network thread
listens on the frame service GUID `…0002…`, but only while a swap chain is assigned —
started in `AssignSwapChain` (`vdd.cpp:1253`), torn down in `UnassignSwapChain`, i.e.
only while DWM is compositing. The host is the connector: `idd_recv_thread_proc`
(`vm_display_idd.c`) opens an `AF_HYPERV` socket to that vsock and renders frames into a
D3D11 window. **Connecting to `:0002` adopts you as the single consumer** — the driver
takes the socket (`listen(…,1)`, backlog 1), sends a full frame, and only notices a
disconnect via a failed send (`vdd.cpp:551-779`). A connection is never free: it starts
a frame session the driver must then clean up. The **non-destructive** readiness signal
is the guest agent's `idd_status`: the Windows agent runs `devcon status
Root\AppSandboxVDD` and sends `idd_status:ok` when the device is "running"
(`tools/agent/agent.c`) — a driver-state check over the agent channel, zero
frame-channel contact.

`vm_display_idd_create(vm, HINSTANCE, HWND main_hwnd)` (`vm_display_idd.h`) is
**self-contained**: its own window thread, message pump, and D3D11 renderer; `main_hwnd`
is used *only* to `PostMessage WM_VM_DISPLAY_CLOSED` on close — the daemon passes `NULL`.

**Linux.** `asb_drm` (`tools/linux/asb_drm`) is a virtual DRM/KMS driver whose card
registers at boot (`asb_drm_drv.c`). `appsandbox-display` (`tools/linux/agent`) is a
userspace daemon that listens on vsock `:2` from boot and, on connect, captures the
primary-plane framebuffer and ships frames in the same wire format. Same single-consumer
shape (`listen(s,1)`, one `capture_loop` at a time), and frames only flow once Mutter is
compositing an active framebuffer. Its readiness signal: the agent reports
`idd_status:ok` once `is_user_session_ready` (Mutter's XWayland auth cookie exists,
`appsandbox-agent.c`) — again over the agent channel, no frame-channel probe.

**macOS.** The display is a `VZVirtualMachineView` in an `NSWindow` (`VzDisplayWindow`,
`vz_display.m`). The `VZVirtualMachine` is **in-process**, so the view can only be
created **inside the daemon process** — there is no frame socket and no separate display
driver, so readiness is simply `running && agentOnline`.

---

## 2. Access model (what gates the feature)

Loopback is **not** an identity or visibility boundary — any local process (any user,
any session, SSH) can connect to `127.0.0.1`, and there is no robust way to identify
the peer of a loopback TCP connection (macOS has no TCP peer-PID at all;
`LOCAL_PEERPID` is Unix-domain-socket only; Windows' `GetExtendedTcpTable` mapping is
a racy snapshot). So we do **not** try to sniff the caller. The real gates:

- **Authorization — who may ask the daemon:** the existing **bearer token** in
  `host.json` (gated by that file's ACL). Unchanged. Not loopback.
- **Frames (B) — who can read the VM's pixels:** the frame connect needs the VM's
  `runtime_id` + HvSocket/vsock access to that partition. The **daemon has both because
  it owns the compute system**; clients never touch the vsock — they go through the
  daemon. So B is satisfied *structurally*; we never enforce it against clients.
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
desktop. A only bites when the daemon has no desktop (service session / headless SSH),
which is exactly when it *should* refuse.

---

## 3. Shared API surface

- `POST   /v1/vms/{name}/display` → open (or focus if already open). Gated, in order:
  `409 no_display` if A is false; `409 not_running` if the VM isn't running;
  `409 display_not_ready` if the display isn't ready yet (see below).
- `DELETE /v1/vms/{name}/display` → close.
- `GET    /v1/vms/{name}/display` → `{"open": bool, "ready": bool}`. **Pollable and
  passive** — it reads flags, opens no window, and never touches the frame channel, so
  polling on an interval can't disturb the display.
- `GET    /v1/vms/{name}` → add `"displayOpen": true|false` (rides on every status).
- SDK (`asb.py`): `open_display`, `close_display`, `display_status` (→ `{open, ready}`),
  `display_ready` (→ `bool`). `c.open_display("vm")` is the `idd.connect("vm")` shape.

**Readiness is a latched flag, never a probe.** Opening the moment a VM is *running* is
too early — the display driver / compositor isn't up, so the window is black. The
*wrong* way to detect "ready" is to connect-test the frame channel: connecting **is**
becoming the consumer (§1), so each poll steals the single consumer slot, triggers a
frame-and-teardown churn, and under load the backlog-1 listener refuses the next connect
— the signal flaps and the real open races the probe's own cleanup into a black window.
Instead, readiness is sourced from the **guest agent** and latched on the host:

```
asb_vm_idd_ready = running && agentOnline && idd_ready
```

`idd_ready` is set when the agent reports `idd_status:ok` (Windows: VDD device
"running"; Linux: user session up) and cleared on agent-offline / stop / reconnect. The
host reads these flags passively; the same predicate backs the GET poll's `ready`.
(macOS has no frame channel or separate driver, so its predicate is `running &&
agentOnline`.)

Put the **A-gate and the readiness flag in the endpoint layer** so both platforms
enforce them before touching any window code (`host_can_show_window()` +
`asb_vm_idd_ready()`).

---

## 4. Windows + Linux implementation

`vm_display_idd_create` is self-contained, and the daemon has what it needs: the
`HINSTANCE` (`run_headless` → `g_hinst`) and `asb_vm_instance(vm)`. The daemon owns its
own table in `headless.c`, **keyed by the VM's stable `unique_id`** (not the array
index — it survives `g_vms[]` compaction):

```c
/* headless.c */
typedef struct { UINT64 vm_id; VmDisplayIdd *disp; } DisplayEntry;
static DisplayEntry g_displays[ASB_MAX_VMS];

/* displayOpen / is-open is computed from the LIVE window state, never a cached flag */
static BOOL display_is_open(UINT64 id) {
    DisplayEntry *e = display_find(id);
    return e && e->disp && vm_display_idd_is_open(e->disp);
}

/* POST /v1/vms/{name}/display -- A-gate, then running, then readiness, then open */
display_reap_stale(v->unique_id);                        /* drop a self-(X)-closed one */
if (!host_can_show_window())  -> 409 no_display;
if (!v->running)              -> 409 not_running;
if (!asb_vm_idd_ready(vm))    -> 409 display_not_ready;  /* the latched flag (§3) */
e = display_find(v->unique_id);
if (e) vm_display_idd_focus(e->disp);                    /* already open -> focus */
else   e->disp = vm_display_idd_create(v, g_hinst, /*main_hwnd*/NULL);

/* DELETE -> display_drop (vm_display_idd_destroy + free slot)
   GET    -> {"open": display_is_open(id), "ready": open || asb_vm_idd_ready(vm)} */
```

**Readiness flag.** `asb_vm_idd_ready` (`asb_core.c`) = `running && agent_online &&
idd_ready`. `idd_ready` is latched in `vm_agent.c` from the agent's `idd_status` line
(set on `ok`, cleared on any other value / agent-offline / stop / reconnect). Nothing
here connects to the frame channel. The IDD auto-probe is disabled in the daemon
(`idd_probe_start` early-returns without a `g_idd_probe_hwnd`), so the daemon never
auto-opens a window the caller didn't ask for; it opens only on an explicit POST.

**The agent supplies the signal (non-destructive, agent channel).** Windows:
`tools/agent/agent.c` already sends `idd_status` on connect (devcon device state).
Linux: `tools/linux/agent/appsandbox-agent.c`'s heartbeat thread sends `idd_status:ok`
the moment `is_user_session_ready` (Mutter up), `not_found` otherwise.

**X-button close.** The `WM_CLOSE` handler (`vm_display_idd.c:2073`) self-tears-down on
its own window thread — stops the recv/audio threads, releases the vsock frame
connection, sets `open = FALSE` — and does **not** free the `VmDisplayIdd` object. So
`vm_display_idd_is_open()` returns FALSE after an X-close, and `displayOpen` (computed
from `is_open`) reflects the close **however it happened** (X, programmatic, VM gone). A
daemon-side **reap** (`display_reap_stale`, at the top of every `/display` request) frees
the dead handle; the status serializer reads `is_open` directly, so `displayOpen` is
correct even before the reap runs. No callback and no message pump are needed.

**Cleanup.** Daemon exit destroys every `g_displays[]` entry; the VM-**delete** handler
`display_drop`s the VM's display first. VM-**stop** is intentionally *not* hooked: the
window persists and its recv thread harmlessly retries (matching how the GUI keeps a
display across a guest reboot); `displayOpen` stays honest via `is_open`, and the user
(or a `DELETE`) closes it.

**A-gate helper (Windows):**
```c
static BOOL host_can_show_window(void) {
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

---

## 5. macOS design (DESIGN ONLY — cannot validate here)

Because the `VZVirtualMachine` is in-process, the window must be created **in the
daemon**, so the daemon must be GUI-capable, so it must run **inside a GUI login
session**. That constraint is immovable (OS reality), but the code delta is contained.

**Readiness.** macOS has no frame channel and no separate display driver — the
`VZVirtualMachineView` binds directly to the in-process VM's framebuffer. So there is
nothing to probe and no recv thread to strand; the readiness predicate is simply
`running && agentOnline` (the agent being the cross-platform "guest is up" signal).
Reject a premature `open_display` with `display_not_ready`, and back the
`GET .../display` `ready` field with the same predicate so the poll-then-open flow and
the `displayOpen` field are identical across platforms. `displayOpen`/close-tracking is
easy: `windowWillClose:` calls back into the core to nil the ref (below), so the same
"compute open-state from the live window, not a cached flag" rule applies.

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
   window (SSH / launchd). */
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

**Open / close / query (`asb_core_mac.m`)** — reuses the window code the
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
        if (!g_vms[idx].agent_online)                     { rc = BACKEND_NOT_READY;   return; }  /* readiness (§5) */
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

## 6. Edge cases (all platforms)

- Not openable → `409`: `no_display` (A false), `not_running`, `display_not_ready`
  (agent not yet reporting the display up). Open when already open → focus + `200`.
  Close when already closed → no-op.
- **VM deleted** while the display is open → the delete handler closes it first
  (`display_drop`). **VM merely stopped** → the window is left in place on Windows (recv
  thread harmlessly retries, like the GUI across a guest reboot); `displayOpen` stays
  honest because it's read from the live window (`is_open`), and the user or a `DELETE`
  closes it. (macOS: nil `g_display_refs[idx]` on the stop/delete transitions.)
- Daemon exit → close all open displays.
- X-button close → the window self-tears-down; `displayOpen` flips false via `is_open`,
  and a reap frees the handle on the next `/display` request (Windows) / `windowWillClose:`
  nils the ref (macOS).
- Single consumer: both the Windows IDDCX driver and the Linux display daemon
  `listen(…,1)`, so at most one display per VM — which the per-VM table enforces.
  Multiple *different* VMs can have displays open at once.

---

## 7. Rollout

1. **Windows half (§4)** — implemented and validated (open-on-ready, close + reopen,
   graceful shutdown + remove; daemon stable throughout).
2. **Linux half (§4)** — implemented and validated. The host-side machinery is shared
   and OS-agnostic; the only Linux-specific code is the guest agent's `idd_status`
   push, which builds on the guest from the pushed branch. Confirmed on fresh Linux
   VMs: both latched ready via the agent's `idd_status` and opened their displays.
3. **macOS half (§5)** — design only, to land behind the A-gate. A macOS daemon without
   a GUI session refuses `open_display`, so shipping the code inert is safe; trusting it
   requires the section-5 run-loop spike on a real Mac.

---

## 8. References (research behind the access model)

- Peer-credentials need a Unix-domain socket / named pipe, not loopback TCP:
  golang/go #27613 (`LOCAL_PEERPID` is UDS-only on darwin); SO_PEERCRED notes
  (UDS-only); Microsoft `GetExtendedTcpTable` (snapshot, racy).
- macOS GUI access is bootstrap-namespace / Aqua-session scoped, not UID-scoped:
  "Accessing the macOS GUI in Automation Contexts" (aahlenst.dev); Apple "Root and
  Login Sessions"; Apple `CGSessionCopyCurrentDictionary()`; `pam_reattach`.
