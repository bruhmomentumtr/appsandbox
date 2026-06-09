# Headless AppSandbox — Implementation Plan

> **BEFORE MERGING `headless-api` -> `main`:** revert the Linux repo-prefetch
> branch in `src/backend_win/asb_core.c` (the `--prefetch-repo --branch
> "headless-api"` line) back to `--branch "main"`. It was pointed at this branch
> so the Linux guest builds its agent/driver source from this revision while the
> branch is in flight; once merged, `main` is the source of truth again.

A `--headless` mode for the existing AppSandbox binary (Windows `appsandbox.exe`,
macOS `AppSandbox.app/Contents/MacOS/AppSandbox`) that runs the core as a
long-lived daemon and exposes a Docker-style local HTTP API. Scripting clients
(Python, Go, anything that speaks HTTP) drive VM lifecycle and **poll status**,
which matters because VMs can take 10–15 minutes to come online.

Every design decision below is grounded in the current code; file:line citations
mark the verified facts the plan depends on.

---

## 1. Goal & shape

- One process owns the core (`appsandbox_core.dll` / `AppSandboxCore.framework`).
- It speaks a Docker-like REST API over **loopback HTTP**; a discovery file gives
  clients the port + token so `asb.connect()` takes no arguments.
- A thin SDK (`asb.py`, Go stub) hides the transport — clients write
  `c.start("myvm"); c.wait("myvm", "online")`.
- **Status is poll-first**: `GET /v1/vms/{name}` returns a derived `state`, backed
  by the core's structured per-VM fields, not log parsing.
- **One shared dispatcher** for GUI and headless (no duplicated verb logic), with
  the small per-mode differences injected via a host interface.
- **One process at a time** (GUI *or* headless), enforced by a crash-safe lock.

---

## 2. Architecture

### 2.1 The shared layers (already shared today)
The entire backend — `asb_core.c`, `hcs_vm.c`, `snapshot.c`, `hcn_network.c`,
`disk_util.c`, `vm_agent.c`, … (~18k LOC on Windows) — is one shared library both
the GUI and headless call. All real VM logic lives here and is **unchanged**.

### 2.2 The `AsbHost` seam (new, small)

> **STATUS — NOT DONE (deemed unnecessary).** This `AsbHost` / `asb_dispatch.c`
> extraction was the plan's step 1, but it never shipped and isn't needed. The realized
> duplication is minimal: the daemon (`headless.c`) is self-contained — it calls the
> core `asb_*` functions directly, keeps its own *cheap* status serializer
> (`append_vm_json`, deliberately distinct from the GUI's snapshot-tree `build_vm_json`
> per §15.4), and its own GUI-mirroring validator (`validate_create`). `ui.c` was not
> refactored. The rest of this section — and §3's `asb_dispatch.c`/`ui.c` rows, §11
> step 1, §15.2, and §15.5's "post-extraction" claim — describe that abandoned design;
> read them as "considered, not built." The one real cost of skipping it:
> `validate_create` must be kept in hand-sync with `web/app.js`'s validators.

The only app-layer code that is currently GUI-coupled is the action dispatch
(`on_webview2_message`, `ui.c:864`) and the outbound serializer (`build_vm_json` +
`send_vm_list`, `ui.c:186-430`). We factor these into a transport-agnostic module
parameterized by a host interface — dependency injection, **no `if(headless)`
branches in shared code**:

```c
/* asb_dispatch.h (new, linked by both GUI and headless) */
typedef struct {
    void (*send)(const wchar_t *json, void *ud);   /* GUI: webview2_post; headless: broadcast */
    void (*before_vm_stop)(int vm_idx, void *ud);  /* GUI: safe_destroy_rdp/idd; headless: NULL */
    void (*after_vm_delete)(int vm_idx, void *ud); /* GUI: compact g_displays[]; headless: NULL */
    void *ud;
} AsbHost;

void asb_dispatch(const wchar_t *json, const AsbHost *host);  /* verb -> asb_*; emits via host->send */
void asb_send_vm_list(const AsbHost *host);
void asb_send_full_state(const AsbHost *host);
```

- **GUI** builds `g_gui_host = { webview2_post, safe_destroy_rdp/idd wrapper,
  display-array-compaction wrapper, NULL }` and its `on_webview2_message` shrinks
  to: handle its GUI-only verbs (`uiReady`, `browseImage`, `connectIddVm`,
  `sshConnect`, `setMinSize`, `enableFeature*`, `selectVm`), else
  `asb_dispatch(json, &g_gui_host)`.
- **Headless** builds `g_headless_host = { pipe/http broadcast, NULL, NULL }`.

macOS is NOT simpler here (corrected after line-by-line verification): the macOS
**core** creates a real display window on the VM's `Running` transition
(`asb_core_mac.m:491-495` → `vz_display.m:12-45` allocates an `NSWindow` +
`VZVirtualMachineView` and `makeKeyAndOrderFront`). So headless macOS needs a small
**core change** — an `asb_mac_set_headless(YES)` flag gating that window creation —
not just an app-layer sink. See §12.

### 2.3 Per-platform daemon threading (verified constraints)
**Windows** (`hcs_init` brands its caller as `g_ui_thread_id`, and that thread gets
a 30s-capped message-pump wait vs `INFINITE` elsewhere — `hcs_vm.c:254-271,661`):
1. Acquire the single-instance lock (§4).
2. On a **dedicated bootstrap thread** (so the worker thread is never
   `g_ui_thread_id`): `prereq_check_all` → `asb_init` → `asb_reconnect_running`
   (`asb_core.c:2446,3609`).
3. Start one **worker/command thread** that serializes all `asb_*` calls (the core
   is not safe for concurrent mutation; `g_vms[]` is lock-free except compaction).
4. Register the five callbacks (`asb_set_state/progress/alert/log/vm_removed`,
   `asb_core.c:2414-2442`) → update an in-memory status snapshot + log ring buffer +
   event bus. **Poll `asb_vm_agent_online()` on a ~1s timer** (it has no callback —
   `vm_agent.c:204` is HWND-only).
5. Start the HTTP server + write the discovery file.
6. On exit: `asb_detach()` (leave VMs running, `asb_core.c:2507`).

**macOS** (every VZ op marshals to the main queue via `run_on_main`,
`asb_core_mac.m:43-46`):
1. In `main.m`, branch on argv (currently ignored, `main.m:5`); set
   `NSApplicationActivationPolicyProhibited`.
2. Acquire the lock; register `asb_mac_set_event_cb`.
3. Run the **main run loop** (`[app run]`) — mandatory or VZ start/stop/state
   callbacks never fire. Commands dispatched onto the main queue.
4. Note: create needs an interactive admin prompt (`[IsoPatchMac preauthorize]`,
   `asb_core_mac.m:658`); for unattended use, run via an `SMAppService` privileged
   helper or as root.

Concurrency is not a bottleneck despite the single worker thread: `asb_vm_start`
fast-returns after spawning `start_vm_thread` (`asb_core.c:3044`) and ISO/Linux
create spawn build threads (`:2749,:2814`), so multiple VMs boot in parallel; the
worker thread only *initiates*.

---

## 3. Existing-code changes (the entire surface)

| File | Change | Nature / risk |
|---|---|---|
| `src/app_win/asb_dispatch.c/.h` (new) | Relocate verb dispatch + `build_vm_json`/`send_*` from `ui.c`, parameterized by `AsbHost` | Faithful move; correctness target = "identical to today", verified by running the GUI |
| `src/app_win/ui.c` | `on_webview2_message` keeps GUI-only verbs + builds `g_gui_host`; WndProc `send_vm_list()` → `asb_send_vm_list(&g_gui_host)` call-site updates | Mechanical; GUI display/WndProc logic stays put |
| `src/app_win/main.c` | `--headless` branch (top of `wWinMain`) + lock in common startup | Additive; normal launches pass no args (verified: `main.c:12`, no self-relaunch) |
| `src/app_win/headless.c` (new) | The daemon (queue, threads, HTTP, status, logs, broadcast) | All-new, dead code in GUI mode |
| `AppSandbox.vcxproj` | add `asb_dispatch.c`, `headless.c`; link `httpapi.lib`, `advapi32` | Build only |
| `src/backend_win/asb_core.c` | (optional, 1 line) make `appsandbox.log` an absolute `%ProgramData%\AppSandbox\` path instead of CWD-relative (`:146,:173`) | Trivial; stable log location |
| macOS mirror | `main.m` branch; `asb_dispatch_mac` extraction from `ui.m`; `headless.m`; Xcode target adds source + `Network.framework` (entitlement `network.server` already present) | Same shape |

Everything else — including all host↔VM communication — is untouched (§9).

---

## 4. Single-instance lock (crash-safe, all launch methods)

One AppSandbox process at a time. Acquire **before** the GUI/headless branch so it
covers GUI-vs-GUI, GUI-vs-headless, headless-vs-headless.

- **Windows:** named mutex `Global\AppSandboxCoreHost`
  (`CreateMutexW`; `ERROR_ALREADY_EXISTS` ⇒ refuse). **Machine-global** because the
  config (`%ProgramData%\AppSandbox\vms.cfg`, `asb_core.c:378`) and HCN networks are
  machine-global, and `asb_init` unconditionally wipes those networks
  (`hcn_cleanup_stale_networks`, `:2464`). The OS auto-releases on crash.
- **macOS:** `flock()` on a file in `~/Library/Application Support/AppSandbox/`
  (config is per-user). OS auto-releases on exit. (LaunchServices blocks a second
  *GUI* instance of the bundle but **not** the direct-binary `--headless` launch, so
  the explicit lock is still required.)

Prefer these OS primitives over a hand-rolled PID lock file (crash-safety). On
"already held": print a clear message and exit; the GUI may activate its window.
This also closes a pre-existing hole — there is **no single-instance guard today**
(verified: no `CreateMutex` in `app_win`), so two GUIs already race.

> **✅ IMPLEMENTATION STATUS (shipped).** Both paths now acquire
> `Global\AppSandboxCoreHost`: the headless daemon in `headless.c run_headless`
> (`ERROR_ALREADY_EXISTS` ⇒ exit 2) and the GUI in `main.c`, before
> `ui_create_main_window` (`ERROR_ALREADY_EXISTS` ⇒ message box + clean exit). So
> GUI-vs-GUI, GUI-vs-headless, and headless-vs-headless are all mutually exclusive —
> "only one at a time" is a guarantee, not a convention. Crash-safety is empirically
> verified (the OS auto-releases the lock when the holder is killed).

---

## 5. The Docker-like API

### 5.1 Transport + discovery
- Loopback HTTP/REST (Windows via `http.sys` / HTTP Server API — no third-party
  dependency; macOS via `Network.framework`). Bound to `127.0.0.1`.
- On startup the daemon writes a **discovery file** (DACL/permission-restricted to
  the user), deleted on clean exit:
  - Windows: `%ProgramData%\AppSandbox\host.json`
  - macOS: `~/Library/Application Support/AppSandbox/host.json`
  ```json
  { "endpoint": "http://127.0.0.1:8787", "port": 8787,
    "token": "k3mZ…", "pid": 12345, "version": "0.1.2", "apiVersion": "v1" }
  ```
- Default port 8787; if taken, bind ephemeral (port 0) and record the real port.
- A bearer **token** gates the loopback port (TCP has no connect-time ACL). The
  token lives in the discovery file; only a user who can read that file gets it.
  (Honest scope: on a single-user box the token mainly blocks *other* local users;
  it's the discovery file's permissions that do the real gating.)

### 5.2 Endpoints (resource model, Docker/Proxmox-style)

| Method + path | Maps to | Notes |
|---|---|---|
| `GET /v1/version` | — | `{product, version, apiVersion, hostOs, capabilities}` (open — no token) |
| `GET /v1/host` | `GetSystemInfo`/`GlobalMemoryStatusEx` + per-VM sums | host cores/RAM/free GB + VM usage |
| `GET /v1/vms` | `asb_vm_count/get` + status | list (cheap fields per VM) |
| `GET /v1/vms/{name}` | status accessors | **the poll endpoint** (derived `state`) |
| `POST /v1/vms` | `asb_vm_create` / `asb_mac_vm_create` | `202 Accepted` (async); validated like the GUI |
| `PUT /v1/vms/{name}` | `asb_vm_set_*` / `asb_mac_vm_edit` | edit; `409` if running **or building** (PUT, not PATCH — PATCH isn't in the http.sys verb enum) |
| `POST /v1/vms/{name}/start` | `asb_vm_start` | `202`; body `{snapIndex,branchIndex,branchName}` (start-from-snapshot / new branch) |
| `POST /v1/vms/{name}/shutdown` | `asb_vm_shutdown` | graceful |
| `POST /v1/vms/{name}/stop` | `asb_vm_stop` | force |
| `POST /v1/vms/{name}/delete` | `asb_vm_delete` / `asb_mac_vm_delete` | `409` if building |
| `GET /v1/vms/{name}/sshInfo` | `ssh_state`/`ssh_port`/`admin_user` | `{host,port,user,sshState,enabled}` |
| `GET /v1/vms/{name}/snapshots` | `asb_snap_*` | tree: `current`, `snapshots[]` (+branches), `baseBranches[]`. **Windows only** → `501` on macOS |
| `POST /v1/vms/{name}/snapshots` | `asb_snap_take` | take `{name?}`; `409` if running |
| `PUT /v1/vms/{name}/snapshots/{s}` | `asb_snap_rename` | rename snapshot/branch `{name,branchIndex?}`; `s = -2` ⇒ a base branch |
| `DELETE /v1/vms/{name}/snapshots/{s}` | `asb_snap_delete` | delete snapshot (`s ≥ 0`; the base node `s < 0` is refused — only its branches delete) |
| `DELETE /v1/vms/{name}/snapshots/{s}/branches/{b}` | `asb_snap_delete_branch` | delete one branch (`s = -2` ⇒ a base branch) |
| `GET /v1/templates`, `DELETE /v1/templates/{name}` | `asb_template_*` | Windows only |
| `GET /v1/logs` | log ring buffer | **single combined log for all VMs** |
| `GET /v1/events` | event bus (SSE) | `state`/`progress`/`agent`/`ssh`/`log`/`alert` |
| `POST /v1/shutdown` | clean daemon stop | `409 vms_active` (+ list) if VMs running/building, unless `{force:true}` |

Keyed by **name** (`asb_vm_find` / `asb_mac_vm_find`), re-resolved every call (array
indices are unstable — `g_vms[]` compacts on delete, `asb_core.c:3181`).

### 5.2a Startup fail-fast (shipped)
Before binding the API, `run_headless` checks three prerequisites and, if any fails,
prints a clear reason (to the launching console via `AttachConsole`, or a dialog if
launched from the GUI) and exits non-zero — it never half-starts:
- **not elevated** (`is_elevated`) → exit `3` ("must be run as Administrator");
- **OS < Windows 11 build 22000** (`os_supported`, via `RtlGetVersion`) → exit `3`;
- **`VirtualMachinePlatform` disabled** (`prereq_check_all`, via DISM) → exit `3`, with the `dism /Enable-Feature` hint.

A second instance (single-instance mutex already held) exits `2`.

### 5.3 Async semantics (verified)
`createVm`/`startVm` return `202` immediately and finish later. Failures of the
template create-and-autostart path surface **only via the alert event**, because
`asb_vm_create` returns `S_OK` even when `hcs_start_vm` fails (`asb_core.c:2962-2979`).
So clients treat create/start as async: `202` then watch status/events.

---

## 6. Status model & polling (the centerpiece)

The daemon serves `GET /v1/vms/{name}` by reading the core's **live structured
fields** on demand (no cache staleness) and collapsing them to one `state`:

| `state` | Condition (verified fields) | Windows source | macOS source |
|---|---|---|---|
| `building` | disk image being built (Windows ISO path) | `building_vhdx` (`asb_vm_is_building()` :3239) + `vhdxProgress` | — (folded into `installing`) |
| `installing` (+`progress`) | **guest OS unattended install — the long phase** | `running && !install_complete` (`install_complete` set at first agent connect, `vm_agent.c:362`) | `!install_complete && install_progress >= 0` + `install_status` |
| `stopped` | exists, off | `!running && !building` | `install_complete && !running` |
| `booting` | powered on, agent not up yet (later, normal boots) | `running && install_complete && !agent_online` | `running && install_complete && !agent_online` |
| **`online`** | guest up + agent connected | `running && asb_vm_agent_online()` (`:3233`) | `running && agent_online` |
| `stopping` | graceful shutdown underway | `shutdown_requested` (`vm_agent.c:219`) | `shutting_down` (`asb_core_mac.m:876`) |

**Correction from verification:** the long 10–15-min phase is `installing`
(`running && !install_complete`), NOT `booting` — after the disk is built the core
**auto-starts** the VM and the *guest* runs unattended setup for minutes
(`asb_core.c:1282`). `install_complete` (`vm_agent.c:362`) is what separates that
first-time install from a later quick `booting`. So the wait spans
`building → installing → booting → online`.

**`create` auto-starts (no separate `start` call):** both ISO (`asb_core.c:1282`)
and template (`:2962`) create auto-start, so `POST /v1/vms` = create + boot +
install. A failed build can **remove the VM** (`:1314-1320` → subsequent
`GET` returns **404**) or leave it **`stopped` with an `alert`** (`:1301-1308`). The
`wait()` helper must treat **404 (vanished)** and **stopped+alert (build OK, start
failed)** as terminal failures — not wait forever. (`start` is only for an
already-created, stopped VM.)

**Reliability — level vs edge (miss-proof guarantee):** all the fields above are
*level* signals (current state that persists), and `GET` reads them **live**, so a
polling client cannot miss a destination state regardless of timing, disconnects, or
restarts — it observes the current value on its next poll. SSE events are *edge*
signals that **can** be missed (late subscribe, dropped connection, bounded-bus
drop), so they are a latency optimization only; polling is the source of truth, and
clients re-poll to resync after any reconnect. `GET` must therefore read live core
fields, never an event-only snapshot. Transient intermediates (e.g. `ssh_state==1`)
may be skipped between polls — wait on the persistent destination. Plus `sshState`
(0/1/2/3, `vm_agent.c:272`) + `sshPort` for SSH readiness.

Python experience:
```python
import asb
c = asb.connect()
while True:
    s = c.status("myvm")             # GET /v1/vms/myvm
    print(s["state"], s.get("progress"))
    if s["state"] == "online": break
    time.sleep(5)
# or:
c.wait("myvm", "online", timeout=1200, interval=5)
```
Polling (not a 15-min-long stream) is the primary mechanism — robust over long
waits, the cloud-SDK convention. SSE `/v1/events` is an optional low-latency add-on.

---

## 7. Logs (single combined stream)

`ui_log`/`asb_log` (`asb_core.c:135,162`) write `appsandbox.log` and call the
registered `AsbLogCallback` — a **flat message string with no VM field**
(`asb_core.h:84`). A single combined log for all VMs is the accepted model, which
matches the code exactly:
- The daemon registers `asb_set_log_callback`, pushes each line (thread-safe — fired
  from any thread) into a ring buffer, and exposes `GET /v1/logs` + an SSE `log`
  event. Zero core change.
- Make the log file path absolute (§3) so the persistent log is findable.

(For *status decisions* clients use the structured per-VM fields in §6, not log
text — keeps long-wait logic reliable.)

---

## 8. Client SDK

`asb.py` (and a Go stub reading the same `host.json`):
`connect()` (zero-arg, reads discovery file) → `create_vm()`, `start()`,
`shutdown()`, `stop()`, `destroy()`, `edit()`, `list()`, **`status()`**,
**`wait()` / `wait_online()` / `wait_stopped()`**, `snapshots…` (Windows),
`logs()`, `events()`. Because it's plain loopback HTTP, clients can skip the SDK and
use `requests`/`curl` against `http://127.0.0.1:<port>/v1/...` directly.

---

## 9. Verified NOT changed: host↔VM communication & status production

Traced line by line — all guest→host signals are produced in the **core** and
exposed as fields (read by the new daemon) and/or HWND posts (handled by the GUI's
WndProc, which stays put):

- **agent online** — `vm->agent_online=TRUE` (`vm_agent.c:356`); field +
  `WM_VM_AGENT_STATUS` (`:204`).
- **shutdown received** — guest `os_shutdown` → `shutdown_requested=TRUE` (`:216-222`).
- **ssh ready** — guest `ssh_ready` → `ssh_state=2` + proxy start (`:271-275`).
- **IDD ready / frames** — IDD probe `WM_VM_IDD_READY` (`asb_core.c:285`);
  `hyperv_video:disabled` → `WM_VM_HYPERV_VIDEO_OFF` (`vm_agent.c:242`).

The refactor touches **none** of `vm_agent.c`, the IDD probe, or the HCS monitor.
The GUI's WndProc handlers (`ui.c:1259-1433`, mostly RDP/IDD display management) stay
in `ui.c`; their only edit is `send_vm_list()` → `asb_send_vm_list(&g_gui_host)`.
Headless leaves the status HWNDs NULL (posts no-op, guarded e.g. `vm_agent.c:203`)
and reads the same fields. The host↔guest channel is byte-for-byte unchanged.

---

## 10. Constraints & non-goals (verified)

- **One owner only** — GUI and headless are mutually exclusive (lock, §4).
- **`MAX_AGENTS=16` < `ASB_MAX_VMS=32`** (`vm_agent.c:59`): VMs past the 16th never
  report `agentOnline`, so they stay `booting` in the derived state — fall back to
  `running` for those, or raise the cap (core change).
- **Alert carries no VM name** (`asb_core.h:84`/`asb_core.c:155`): async failures
  can't be perfectly attributed if two overlap; acceptable, or add a 1-line
  `asb_vm_unique_id` export later for stable-id keying.
- **Display headless is out of scope** — RDP/IDD viewers are GUI/window-bound
  (`vm_display.c`, `vm_display_idd.c`); expose VM lifecycle/status, not pixels.
- **Machine-reliable per-VM logs are out of scope** — would require tagging every
  `ui_log` call site; single combined log is sufficient (§7).
- **macOS unattended create** needs a privileged helper (`preauthorize`, §2.3).

---

## 11. Implementation order

1. **Extract `asb_dispatch.c` + `AsbHost`; wire the GUI to it via `g_gui_host`.**
   Build and run the GUI; confirm byte-identical behavior. (Riskiest existing-code
   change — done and verified in isolation, no headless yet.)
2. **Single-instance lock** in common startup (both GUI and headless paths).
3. **`headless.c` skeleton:** `--headless` branch, lock check, bootstrap thread
   (`asb_init`/`reconnect`), worker queue, register the five callbacks → status
   snapshot + log ring buffer + agent-online poll.
4. **`http.sys` server + discovery file + token**, then `GET /v1/version`,
   `GET /v1/vms`, `GET /v1/vms/{name}` (derived `state`). Smoke-test polling against
   any already-built VM.
5. **Mutating verbs** (create/start/shutdown/stop/delete/edit), snapshots, templates,
   `GET /v1/logs`, SSE `/v1/events`.
6. **`asb.py` SDK** with `status()`/`wait()` + examples (create/start/poll/stop/destroy).
7. **macOS port:** `main.m` branch, **split `ui_set_webview`** (decouple
   `asb_mac_init` from the window), `asb_dispatch_mac` extraction, `headless.m`
   (run loop + `asb_mac_set_event_cb`), the **`asb_mac_set_headless` core change**
   (§12), same HTTP/SDK; capability flag turns snapshots/templates off;
   `SMAppService` for unattended create.

---

## 12. Status delivery: events vs polling (verified per platform)

Verified by tracing the producers line by line. The two backends differ, and it
shapes the daemon's status code.

### 12.1 Windows — hybrid (events + targeted polling)
`asb_set_*_callback` are genuine push events:
- **`AsbStateCallback`** (running/stopped) — fired from `asb_vm_stop` (`:3128`),
  `asb_vm_shutdown` (`:3091`), `asb_vm_create` (`:2751,2824,2978`),
  `start_vm_thread` (`:948`), the build thread (`:1327`), and
  `asb_hcs_state_changed` on self-exit (`:744`, HCS threadpool thread).
- **`AsbProgressCallback`** (VHDX build %, `:1105,2053`), **`AsbAlertCallback`**,
  **`AsbLogCallback`**, **`AsbVmRemovedCallback`** — all events.

**No callback (the gap):** `agent_online`, `ssh_state`, and `install_complete` are
**not** events — set on the VM struct (`vm_agent.c:356,272,362`) and announced only
as `WM_VM_AGENT_STATUS` posted to an **HWND** (`vm_agent.c:204`). A headless daemon
therefore must, for those three:
- **v1:** poll the fields (`asb_vm_agent_online()` / `inst->ssh_state`) on a ~1s
  worker-thread timer, **or**
- **v2:** stand up a message-only window (`HWND_MESSAGE` + pump thread) registered
  via `vm_agent_set_hwnd` to receive `WM_VM_AGENT_STATUS` as events (one message
  carries both `agent_online` and `ssh_state`).

These three are exactly the signals that distinguish `installing` / `booting` /
`online`, so they're the load-bearing ones for the 10–15-min wait.

### 12.2 macOS — all events, zero polling
The macOS core routes everything through the single `g_event_cb`: `STATE_CHANGED`
(`asb_core_mac.m:485,500`, via `handle_vm_state_change` on the main queue),
`PROGRESS`/`INSTALL_STATUS` (`:516-518`), **`AGENT_STATUS` with `int_value=online`**
(`:355,390,429`, delivered on main via `vm_agent_mac.m:353,364`), `ALERT`, `LOG`,
`LIST_CHANGED`. So the macOS daemon gets agent-online **as an event** — **no
polling**. The decisive signals (`running`, `agent_online`) are confirmed
main-thread, so a daemon that marshals reads to the main queue is race-free on them.
(Verified: `IsoPatchMac` dispatches its progress + completion blocks to the **main
queue** — `iso_patch_mac.m:137,149,224` — so `update_install_progress`/`finish_install`
run on main; no race.)

### 12.3 macOS required core change (the one place macOS needs more than Windows)
On the `Running` transition the macOS **core** allocates and front-orders a real
`NSWindow` + `VZVirtualMachineView` (`asb_core_mac.m:491-495` → `vz_display.m:12-45`,
`showDisplay` = `showWindow:` + `makeKeyAndOrderFront:`), and tears it down on
`Stopped` (`:475-479`) / `Delete` (`:911-914`). Unlike Windows (where the *app*
creates display windows), this is in the core start path, so:
- A headless daemon calling `asb_mac_vm_start` triggers core-side window creation —
  which **fails in a session-0 LaunchDaemon (no window server)** and pops a stray
  window in a GUI-session agent.
- **Required:** add `asb_mac_set_headless(YES)` (or a per-VM flag) gating
  `asb_core_mac.m:491-495`. Teardown paths become harmless no-ops.
- **Consequence:** with the flag, VZ itself needs no window server, so the daemon can
  run headless — but realistically as a **Prohibited-policy login-session agent**,
  and only as a true session-0 LaunchDaemon if the flag fully removes the
  window-server dependency (it should, since that display creation is the only
  window-server use in the VM path).

### 12.4 Summary
| Signal | Windows | macOS |
|---|---|---|
| running/stopped | event (`AsbStateCallback`) | event (`STATE_CHANGED`) |
| build/install progress | event (`AsbProgressCallback`) | event (`PROGRESS`/`INSTALL_STATUS`) |
| alert / log / removed | event | event |
| **agent_online** | **poll** (or message-only window) | **event** (`AGENT_STATUS`) |
| **ssh_state** | **poll** (or message-only window) | event (`AGENT_STATUS`) |
| display on start | app-created (headless skips) | **core-created → needs `set_headless`** |

---

## 13. Test plan

> **STATUS (shipped).** A self-contained test suite now lives under
> `tools/headless-api/tests/`: `run_all.py` runs no-VM static checks
> (`test_host_and_validation.py`), then the full per-VM lifecycle (`vm_lifecycle.py`)
> concurrently across a Linux and a Windows spec, then the template lifecycle
> (`test_windows_template.py`). The per-VM lifecycle also folds in SSH key auto-deploy
> (build with `sshDeployKey`, assert `keyDeployed`/`sshState 4`, key-only login). Every
> test builds and destroys its own `brk-*` throwaway VMs (never touches any pre-existing
> VMs or templates). It is plain-stdlib Python driving `asb.py` over HTTP — not the
> `pytest`/C-golden-test stack the plan below originally sketched (the golden test was
> for the abandoned `asb_dispatch` extraction, §2.2).

The repo has no test framework today (only dev scripts under `tools/`). The
pragmatic stack: **pytest** driving the Python SDK against the running daemon for
integration (the SDK is Python anyway), plus a small **C golden-test** harness for
the one refactor that touches working GUI code. Integration tests need elevation +
Hyper-V/VZ and real VMs, so the bulk runs on a dev box with the prereqs; CI needs a
virtualization-capable runner.

### 13.1 Refactor-safety net (the #1 risk — the `asb_dispatch` extraction)
Before extracting, capture a **baseline**: feed a fixed script of action messages
through the current `on_webview2_message` with a *recording* sink and save the
emitted JSON + the sequence of `asb_*` calls. After extraction, run the same script
through `asb_dispatch` with a recording `AsbHost` and **diff against the baseline** —
any difference is unintended drift. Pair with a **manual GUI smoke test** (launch the
GUI, create/start/stop/snapshot/delete a VM) to confirm identical behavior. This is
what proves the move didn't change the GUI.

### 13.2 Unit / component (C, pure logic)
- Derived-`state` mapping: table of `(running, building, install_complete,
  agent_online, shutdown_requested)` → expected `state`, incl. the
  `installing` vs `booting` distinction.
- Discovery-file write→read round-trip; token generation + bearer-auth check
  (valid/invalid/missing).
- Single-instance lock: acquire succeeds; a second acquire is rejected; **kill the
  first via `TerminateProcess` and confirm the next acquire succeeds** (crash-release).
- JSON request parse / response build for each verb.

### 13.3 Integration (pytest → SDK → daemon → real core/VMs)
- `GET /v1/vms` lists the existing VMs; `GET /v1/vms/{name}` returns a derived `state`.
- **Lifecycle on an existing VM:** `start` → poll to `online` → `shutdown` → poll to
  `stopped`. Assert each `wait()` converges.
- **Create lifecycle (throwaway VM):** `POST /v1/vms` → poll `building → installing →
  online` → `DELETE` → next `GET` is `404`.
- **Concurrency:** two clients `start` two VMs simultaneously → both reach `online`;
  assert status polls keep returning during a slow op (don't block behind the worker).
- **Miss-proof:** open the SSE stream *late*, and disconnect/reconnect mid-wait →
  assert polling still converges to `online` (validates level-vs-edge, §6).
- **Failure paths:** bad config → `400` (sync validation); async start failure →
  `alert` event + VM `stopped` or `404`; assert `wait()` exits terminal, not hang.

### 13.4 Crash / recovery
Kill the daemon (`TerminateProcess`) mid-operation; restart; assert: lock released,
`asb_reconnect_running` re-attaches surviving VMs, `vms.cfg` intact. Documents the
known unclean-shutdown caveats (VM lifetime tied to HCS handle ownership; non-atomic
`vms.cfg` — see §4) and validates the atomic-write fix if added.

### 13.5 Cross-platform
The same pytest *contract* suite runs against the Windows and macOS daemons.
Snapshot/template tests are skipped on macOS via the `capabilities` flag. macOS adds:
the `asb_mac_set_headless` flag actually suppresses the display window (start a VM
headless and assert no window / no window-server error), and agent-online arrives as
an event.

### 13.6 Manual / exploratory (not automatable)
Real 10–15-min Windows install from an ISO (elevation + real install); macOS
`preauthorize` admin prompt; UAC elevation; macOS session-0 LaunchDaemon vs
login-session agent; and confirming a headless `start` does not hang or pop a window.

---

## 14. GUI feature parity — what the headless interface does NOT do

The headless API covers all VM **management** (create/start/shutdown/stop/delete,
config edit, snapshots [Windows], templates [Windows], status, logs). It omits the
**interactive/visual** layer:

| GUI capability | Headless | Source |
|---|---|---|
| See the VM screen (RDP/IDD on Win; `VZVirtualMachineView` on Mac) | **No** | `vm_display.c`/`vm_display_idd.c`; `vz_display.m` |
| Keyboard/mouse input to the VM | **No** | tied to the display window |
| Clipboard sync (host↔guest) | **No** | `vm_clipboard.c` owns the host clipboard |
| Audio (guest → host) | **No** | `appsandbox-audio.exe` / `vz_display.m:43` |
| Interactive SSH console | **Reshaped** → `sshInfo` (host/port/user); client SSHes itself | GUI `sshConnect` spawns a terminal (`ui.c:984`, `ui.m:418`) |
| ISO/ipsw file picker | **No** — client passes the absolute path | `browseImage` (`ui.c:1035`, `ui.m:328`) |
| Enable Hyper-V / reboot host | **No (checks only)** | `enableFeature*` (`prereq.c`) |
| Window chrome (selection, min-size, tray, Event Log window) | **No (N/A)** | `selectVm`/`setMinSize`/tray/`EventLogWindow` |
| Rich create-form UX (smart defaults, live validation, dropdowns) | **No** — caller supplies raw config | `app.js` (client-side) |

- **Recoverable later (deferred, not impossible):** screen + input via the windowless
  `AsbDisplay` (Win: `asb_display.c` `screenshot`/`mouse`/`key`/`type_text`; Mac would
  need a framebuffer/VNC path) or exposing the RDP pipe path for the client's own
  viewer.
- **Intentionally never:** enabling Hyper-V / rebooting the host from a daemon; host
  clipboard/audio; tray/window chrome.
- **Platform-absent:** snapshots & templates do not exist on macOS (`ui.m:429-436`),
  so those endpoints return `501` there.

---

## 15. Implementation rules & decisions

The detailed rules that the rest of the plan depends on, verified against the code.

### 15.1 Threading rules (Windows) — a checklist
- **Bootstrap thread parks for the process lifetime — it must NOT exit.** It runs
  `prereq_check_all → asb_init → asb_reconnect_running`. `hcs_init` brands its caller
  as `g_ui_thread_id` (`hcs_vm.c:661`); on that thread every HCS op takes a 30s-capped
  `PeekMessage` path vs `INFINITE` elsewhere (`:254-271`). If the bootstrap thread
  exits, Windows can **reuse its thread ID**, and any later thread that matches it and
  calls `hcs_exec_and_wait` silently inherits the 30s cap. So park it on an event for
  the process lifetime, or repurpose it as the v2 message-only-window pump thread
  (which has its own `GetMessage` loop — a fine home for `g_ui_thread_id` since it
  never makes VM-op calls).
- **The worker thread is the single owner of all mutating `asb_*` calls, and is a
  DIFFERENT thread from the bootstrap/`g_ui_thread_id` thread** (so it always takes
  the clean `INFINITE` HCS path).
- **The 5 registered callbacks must be strictly non-blocking and re-entrant-safe.**
  They fire on three kinds of thread: the worker thread *synchronously mid-command*
  (`asb_vm_stop:3128`, `shutdown:3091`, `create:2751/2824/2978`, `snap:3383`,
  `start` warm path `:3062`), HCS threadpool threads (`asb_hcs_state_changed:744`),
  and build threads (`:948,1327`). Each callback may only **copy + push to a bounded
  non-blocking bus / update the status snapshot under a short lock, then return**. It
  must never: re-enter the core (`asb_*`), enqueue-and-wait on the worker thread
  (deadlock — it may *be* the worker thread), or block on a lock the worker holds.
  **Empirically verified (2026-06-08, elevated ctypes spike, start+stop of the `linux`
  VM):** callbacks fire in a process with **no message pump** (so none is required) —
  on background threads for async firings (start-complete + exit, both tid≠main) AND
  **on the calling thread synchronously** when a command is issued from it
  (`asb_vm_stop` fired the callback on the main thread mid-call), confirming the
  deadlock hazard above is real, not theoretical.
- **macOS:** the equivalent "worker thread" is the **main run loop** — marshal all
  commands and all status reads onto the main queue (`run_on_main`); the event
  callback is already main-thread for the data-bearing events (§12.2).

### 15.2 The `asb_dispatch` extraction — not a pure cut-paste for 3 verbs
`stop/shutdown/delete` carry inline GUI state that must be relocated, not moved:
- Drop the redundant `inst->shutdown_requested = TRUE` pre-write (`ui.c:945-946`) —
  the core sets it (`asb_vm_shutdown:3087`).
- Move `g_selected_vm = -1` (delete) and the display teardown (`safe_destroy_rdp/idd`)
  into the GUI host via `before_vm_stop` / `after_vm_delete` hooks; headless passes
  `NULL` hooks. Everything else moves verbatim.

### 15.3 Concurrency & isolation — the honest limit
The single worker thread serializes the **daemon's own** `asb_*` calls (no torn
state from the daemon's concurrent HTTP requests). It does **NOT** serialize against
the **core's own background threads** — `vhdx_create_thread`, `linux_create_thread`,
`start_vm_thread`, and `asb_hcs_state_changed` mutate/compact `g_vms[]` under the
**private `g_cs`** (`asb_core.c:1315,718,3181`), a `CRITICAL_SECTION` the app layer
cannot take. The daemon's status reads (`build_vm_json`, `asb_vm_agent_online` — a
bare field read at `:3233`) don't hold `g_cs`, so they can race a concurrent
compaction (a microsecond torn-read window). **This is the exact race the GUI already
ships with** (it reads `build_vm_json` on the UI thread without `g_cs`), tolerable
because compaction is rare (delete / failed-create / template-finalize).
- **Decision:** match the GUI for v1 (accept the rare race). A fully race-free design
  needs a small **core change** — expose `g_cs` or add a thread-safe list-snapshot
  accessor — which is out of scope for v1.
- **macOS** is analogous: `g_vms[]` is compacted on the main thread
  (`asb_mac_vm_delete:929-943`); reads on the main queue are safe, off-main reads race.

### 15.4 Poll-endpoint split (keep polling cheap)
`build_vm_json`'s snapshot loop does **disk I/O per snapshot** — `get_file_size_bytes`
on every base/snapshot/branch VHDX (`ui.c:226,245,267,284`) plus `snapshot_find_current`.
A client polling every 5s for 15 min would otherwise open every snapshot VHDX each
poll. So:
- **`GET /v1/vms/{name}` (the hot poll path) returns cheap field reads only:** `name`,
  derived `state`, `running`, `agentOnline`, `installComplete`, `vhdxProgress`/
  `installProgress`, `sshState`, `sshPort`, `ramMb`/`hddGb`/`cpuCores`, `gpuMode`,
  `networkMode`. No disk I/O.
- **`GET /v1/vms/{name}/snapshots`** returns the full snapshot tree + sizes, on demand.
- **`GET /v1/vms`** returns the cheap fields per VM (no snapshot sizes).

### 15.5 Sharing inventory (single-source guarantee, explicit)
| Category | ~Count | Contents |
|---|---|---|
| **Used unmodified** | ~80+ | Entire core (~70 `asb_*` exports + all internal core fns) + `jb_*`/`json_get_*` + `build_vm_json` + `asb_dispatch` (post-extraction) |
| **Used with modification** | ~8–10 | `send_*` sink indirection (~6), the `on_webview2_message` split, the 3 verb handlers (§15.2), WndProc `send_vm_list()` call sites |
| **Duplicated VM logic** | ~0 | none |
| **New (not a duplicate)** | ~20–30 | `headless.c` daemon: lock, bootstrap/worker threads, queue, status snapshot + derived state, log ring buffer, agent-poll, HTTP server + routes, discovery file, token, `AsbHost` impl |

The "new" daemon code is **parallel plumbing** to the GUI's window/WebView2/pump layer
(transport, the 5 callbacks, the host loop), not duplicated logic — every piece is a
thin adapter that funnels into the *same* shared `asb_dispatch`/core, so it can't drift
on behavior. The GUI's "version" of most of that plumbing is the OS/SDK (message pump,
WebView2), not functions we maintain.

### 15.6 Optional core hardening (recommended for an unattended daemon)
- **Atomic `vms.cfg` write.** `save_vm_list` is a truncate-and-rewrite
  (`asb_core.c:388`, `_wfopen_s(..., "w")`) — a crash mid-save can tear the file. Fix:
  write `vms.cfg.tmp` then `MoveFileExW(tmp, cfg, MOVEFILE_REPLACE_EXISTING)`.
- **Absolute log path.** `ui_log`/`asb_log` open `appsandbox.log` **CWD-relative**
  (`asb_core.c:146,173`). Fix: `%ProgramData%\AppSandbox\appsandbox.log` so the
  daemon's log is findable. 1-line each.
- **`asb_vm_unique_id` export** (1-liner) — stable id keying + correct alert
  attribution (alert carries no VM, `:155`).
- **macOS `asb_mac_set_headless`** (§12.3) — required, not optional, for headless macOS.

---

## 16. Deployment & privilege model

The daemon **always runs elevated** — it is the same `appsandbox.exe`, which carries
`UACExecutionLevel=RequireAdministrator` (verified, `AppSandbox.vcxproj`), and HCS/HCN
+ `vmms_cert_ensure` (`vmms_cert.c`, writes `LOCAL_MACHINE`) require admin anyway.
Consequences:
- **Elevation settles two would-be unknowns:** an elevated process has
  `SeCreateGlobalPrivilege` so the `Global\` single-instance mutex (§4) just works, and
  it binds `http.sys` on `127.0.0.1` **without a URL-ACL reservation** (URL ACLs exist
  to delegate binding to *non-admin* processes). Neither is an open question.
- **Unattended autostart = Windows Service** (LocalSystem or an elevated service
  account). Launching `appsandbox.exe --headless` interactively would otherwise pop a
  UAC prompt each time; a Service runs elevated with no prompt and survives logout.
  macOS equivalent: `SMAppService` privileged helper (also needed for the create
  `preauthorize`, §2.3).
- **Privileged daemon, unprivileged client (the `dockerd`/`docker` model).** The client
  (your Python script) may run **non-elevated**, so the discovery file's DACL must
  grant the **interactive user read** — not just SYSTEM/admin — or the client can't read
  its own token. Honest caveat that follows: once the daemon is elevated and the token
  is user-readable, **any same-user process can drive privileged VM operations**. That
  is acceptable on a single-user box (the user is already admin) and matches Docker's
  posture, but it is the real boundary — not the token, which is near-theater against
  same-user code. If the daemon runs as **LocalSystem**, build the pipe/file DACL with
  the explicit `SY` + interactive-user-SID split (resolve the SID at runtime), not a
  blanket "interactive user".

## 17. Verification status

Honest ledger as of 2026-06-08 — what is proven and how.

**Code-verified (read line-by-line, cited):** the full Windows lifecycle
(create/start/stop/shutdown/delete + the auto-start, `asb_core.c:1223`,1282,2962); the
`g_ui_thread_id` 30s trap (`hcs_vm.c:254-271,661`); the 5 callbacks and where each
fires (sync worker paths + `asb_hcs_state_changed:744` + build threads); status as
live level fields; host↔VM comms in `vm_agent.c` (untouched by the refactor); the GUI
callbacks are pure marshalers (`ui.c:1135`) with an explicit off-UI-thread check
(`:1123`); `vms.cfg` non-atomic (`:388`); SSH-MSI uses host-exe dir
(`disk_util.c:622`); the macOS lifecycle, name-keyed events + their main-queue
threading (`iso_patch_mac.m:137,149,224`), the core-creates-the-display-window finding
(`asb_core_mac.m:491-495` → `vz_display.m`), and that `vz_vm.m` is **AppKit-free** (only
`vz_display.m` touches the window server).

**Empirically verified (ran it):** the HCS state callback fires with **no message
pump** and on background threads for async firings + synchronously on the calling
thread for in-command firings — confirmed via an elevated ctypes spike that started +
stopped the `linux` VM (§15.1). The elevated ctypes-host model (init/reconnect/start/
stop/detach) works end-to-end.

**Resolved by the elevation requirement (not assumptions):** `Global\` mutex privilege;
`http.sys` loopback bind without a URL-ACL (§16).

**One remaining runtime item — not testable from Windows:** whether Apple's
`VZVirtualMachine` framework itself needs a GUI session at runtime. Code is in our
favor (`vz_vm.m` AppKit-free; `asb_mac_set_headless` gates the only window-server use),
so the residual is framework-internal — confirm on a Mac during the macOS phase.

**Built, run, and tested (current status):** the Windows daemon is compiled into
`appsandbox.exe` (`headless.c`), runs against the real VMs, and is covered by the
`tools/headless-api/` regression suite (static + concurrent per-VM Linux/Windows + template).
The `asb_dispatch` extraction was deemed unnecessary and skipped (§2.2). Shipped since
the original plan: the startup fail-fast (admin / Win11 22000 / VirtualMachinePlatform),
the `POST /v1/shutdown` active-VM guard, `GET /v1/host`, base-branch snapshot
list/rename/delete, GUI-mirrored input validation (incl. the username only-dots/spaces
rule + whitespace trim), an edit-while-building guard, and an SSE `/v1/events` stream.

**SSH key deploy (shipped, agent-driven — both GUI and headless).** `sshDeployKey`
(create option, needs `sshEnabled`) generates an AppSandbox ed25519 keypair once
(`%ProgramData%\AppSandbox\ssh\`) and the guest agent writes the public key into the
guest's `authorized_keys` once SSH is up (Windows: `administrators_authorized_keys` +
the strict ACL; Linux/macOS: `~/.ssh/authorized_keys`), reporting a new
`ssh_key_deployed` status -> `sshInfo` `keyDeployed:true` and `sshState 4`. Uniform
across all three OSes because it rides the existing agent channel, not the per-OS
build paths. (Windows is verified end-to-end; Linux/macOS guest agents are built from
the GitHub source on VM creation, so they go live once the branch is pushed.)

**Still future / undecided:** the macOS daemon (§12.3 `asb_mac_set_headless`), and the
optional `GET /v1/logs` + `agent`/`ssh`/`log` SSE events — only `state`/`progress`/`alert`
are emitted today (agent-online and ssh-readiness remain observable by polling
`status()` fields, per the level-vs-edge model in §6).
