# AppSandbox headless SDK (`asb.py`)

A small, dependency-free Python client for driving AppSandbox programmatically —
create, configure, snapshot, and run GPU-enabled VMs without the GUI.

> Design rationale, architecture, and the full daemon decision log live in
> [`PLAN.md`](PLAN.md); this README is the usage/API reference.

```python
import asb

c = asb.connect()                      # auto-discovers the running daemon
c.create(name="dev", osType="Linux", imagePath=r"C:\ISOs\ubuntu.iso",
         ramMb=4096, cpuCores=2, adminUser="user", adminPass="test123")
c.wait_online("dev")                   # blocks through build + install + boot
print(c.ssh_info("dev"))               # {'host': '127.0.0.1', 'port': ..., ...}
c.shutdown("dev")                      # graceful ACPI power-off
c.delete_vm("dev")
```

---

## How it works

`appsandbox.exe --headless` (Windows) or
`sudo AppSandbox.app/Contents/MacOS/AppSandbox --headless` (macOS) starts a
**single-owner daemon**. It hosts the AppSandbox core (`appsandbox_core.dll` /
`AppSandboxCore.framework`) for the lifetime of the process and exposes the
SAME Docker-style local HTTP/JSON API on both platforms. The daemon *owns* the
VMs: they run as long as it runs (exit terminates them), and the API is just a
remote control for the same core the GUI drives.

- **Loopback only.** The listener binds `http://127.0.0.1:<port>` (`http.sys`
  on Windows; a plain BSD socket on macOS). Nothing is exposed off-box.
- **Discovery file.** On startup the daemon writes `host.json`
  (`%ProgramData%\AppSandbox\` on Windows;
  `~/Library/Application Support/AppSandbox/` on macOS) with the endpoint,
  port, and a random per-run bearer token. `asb.connect()` reads it, so you
  never hard-code a port or token.
- **Single instance.** A global mutex (Windows) / `flock` (macOS) guarantees
  one AppSandbox at a time — GUI or daemon (a second `--headless` exits with
  status 2). One owner, one source of truth.
- **Serialized core.** The HTTP receive loop *is* the single command thread:
  one request runs to completion before the next, so all core calls are
  serialized — no interleaving, no races against the core.
- **Capabilities, not API forks.** `version()["capabilities"]` advertises what
  the host supports (snapshots/templates are Windows-only); unsupported routes
  return `501`. Everything else — status model, validation, SSE, SSH key
  deploy — is identical.

### Security model — read this before "hardening" it

The API is **admin-gated, and that gate is functional, not a security boundary.**
Creating, starting, and snapshotting VMs go through the Host Compute Service
(HCS), which requires elevation; binding the `http.sys` URL likewise needs
elevation. So the daemon must run elevated, and only an administrator can launch
it. The bearer token exists to stop *other processes* from stumbling onto the
port, not to defend against an authenticated administrator on the same machine —
that is explicitly **out of scope**. The threat model is single-user. Do not add
auth layers, DACL tightening, or token ceremony that the local single-user model
doesn't justify.

---

## Platform support at a glance

The HTTP/JSON interface is identical on both hosts; what differs is the **guest
OSes** each host can run and a few **host features**. Guest support is enforced
in create-validation (wrong `osType` → `400`); the host-feature row mirrors
`version()["capabilities"]` (unsupported routes → `501`).

| | Windows host | macOS host |
|---|:---:|:---:|
| **Windows guest** | ✅ | ❌ |
| **Linux guest** (Ubuntu) | ✅ | ❌ |
| **macOS guest** | ❌ | ✅ (Apple silicon) |
| Snapshots & branches | ✅ | ❌ `501` |
| Templates (`isTemplate`, create-from-template) | ✅ | ❌ `501` |
| GPU, NAT/network modes, SSH server, SSH-key auto-deploy, SSE events | ✅ | ✅ |

Only `snapshots`/`templates` are advertised in the `capabilities` object; guest
OS is host-fixed (Windows host → Windows + Linux; macOS host → macOS only), so
a client picks its `osType` from `version()["hostOs"]`, not from `capabilities`.

## Requirements

**Windows host** (runs Windows + Linux guests):

| Requirement | Why |
|---|---|
| **Windows 11, build 22000+** | The bundled virtual display (VDD) and audio (VAD) drivers are Windows-11-only (their INFs target `10.0…22000`; the VDD uses the Win11 UMDF reflector). |
| **Administrator** | HCS VM operations and the `http.sys` bind both require elevation. |
| **`VirtualMachinePlatform` feature** | The HCS backend the core uses. Enable with `dism /online /Enable-Feature /FeatureName:VirtualMachinePlatform /All` then reboot. |

**macOS host** (runs macOS guests only — Virtualization.framework):

| Requirement | Why |
|---|---|
| **macOS ≥ the build's minimum** (`LSMinimumSystemVersion`) | The daemon runtime-checks the OS version (a direct `--headless` launch bypasses the LaunchServices gate that protects the GUI). |
| **Virtualization supported** | `VZVirtualMachine.isSupported` — Apple silicon + the Hypervisor entitlement. False on Intel Macs (no macOS-guest support). |
| **`sudo`** | VM creation stages the guest disk as root, and a password prompt would not be scriptable — so the daemon requires elevation up front and is then fully non-interactive. It still operates on the **invoking user's** VM registry and restore-image cache (`SUDO_USER`), and hands every file it creates back to that user, so the GUI keeps working afterwards. |

On either platform a missing prerequisite makes `--headless` **fail fast with a
clear message** and a non-zero exit — it never half-starts:

- Windows: not elevated → "must be run as Administrator…"; unsupported OS →
  "requires Windows 11, build 22000 or newer"; feature off → "requires the
  'VirtualMachinePlatform' Windows feature…"
- macOS (each checked at runtime, exit 3): unsupported OS → "unsupported macOS —
  detected `<ver>`, requires `<min>` or newer"; no virtualization → "Virtualization
  is not supported on this Mac"; not root → "must be run elevated … run it with
  sudo: …"; `SUDO_USER` set but unresolvable → aborts rather than using the wrong registry
- a second instance (GUI or daemon already running) → exit 2

---

## Getting started

1. **Start the daemon**:
   ```
   # Windows (elevated shell)
   appsandbox.exe --headless

   # macOS
   sudo /Applications/AppSandbox.app/Contents/MacOS/AppSandbox --headless
   ```
   On Windows it runs in the foreground-less GUI subsystem; on macOS it runs in
   the foreground of your terminal (Ctrl-C = clean shutdown). Diagnostics go to
   `headless.log` next to the discovery file.

2. **Connect** from Python (stdlib only — no `pip install`):
   ```python
   import asb
   c = asb.connect()
   print(c.version())   # {'product': 'AppSandbox', 'version': '0.1.2', ...}
   ```

`asb.py` has no third-party dependencies; copy it next to your script, or add
`tools/headless-api` to `sys.path` (the examples do the latter).

---

## VM lifecycle

A VM's `state` is **derived** from the core's flags, so it's always live —
there's no cached status to go stale:

```
building → stopped → installing → booting → online → stopping → stopped
   │           ▲          (running = true ─────────────┘)
   └─ vhdx build         agent connects at "online"
```

| state | meaning |
|---|---|
| `building` | the VHDX is being created/staged (the build worker holds the disk) |
| `stopped` | powered off |
| `installing` | powered on, guest OS still installing (`installComplete` = false) |
| `booting` | installed and powered on, guest agent not yet connected |
| `online` | guest agent connected — fully up |
| `stopping` | graceful shutdown requested, not yet powered off |

`create()` is **async and auto-starts** the VM, so a fresh create walks
`building → installing → booting → online` on its own. Watch it with
`wait()`/`status()` (poll-based and miss-proof) or `events()` (push, for UIs).

---

## API surface

`connect()` returns a `Client`. Query methods return parsed JSON; lifecycle
methods return `(http_status, body)` so you can branch on the status code.

### Queries
| Method | Returns |
|---|---|
| `version()` | `{product, version, apiVersion, hostOs, capabilities}` |
| `host()` | `{hostCores, hostRamMb, freeGb, vmCores, vmRamMb, vmHddGb}` (capacity + what running/declared VMs use) |
| `list()` | list of VM status objects |
| `status(name)` | one VM's status (raises `KeyError` on 404) |
| `ssh_info(name)` | `{host, port, user, sshState, enabled, keyDeployed}` (loopback-forwarded SSH; `sshState 4` = ready + key deployed) |
| `templates()` | `[{name, osType}, …]` |
| `snapshots(name)` | list of `{index, name, branchCount, branches:[…]}` |
| `snapshots_full(name)` | the above **plus** `current: {snapIndex, branchIndex}` |

A **status object** has: `name, osType, state, running, agentOnline,
installComplete, building, progress, sshState, sshPort, ramMb, hddGb, cpuCores,
gpuMode, networkMode`.

### Lifecycle  *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `create(**cfg)` | create + auto-start (validated; see below) |
| `edit(name, **fields)` | change config — **VM must be stopped** (else 409) |
| `start(name, snap_index=-1, branch_index=-1, branch_name=None)` | power on; optionally boot a snapshot/branch |
| `shutdown(name)` | graceful ACPI power-off |
| `stop(name)` | force power-off (pull the plug) |
| `delete_vm(name)` | remove the VM and its disk (refused 409 while building) |
| `delete_template(name)` | remove a template |
| `shutdown_daemon(force=False)` | stop the daemon (refused 409 if VMs are active unless `force=True`) |

### Snapshots & branches *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `snap_take(name, snapname=None)` | snapshot — **VM must be stopped** (else 409) |
| `snap_rename(name, index, new_name, branch_index=-1)` | rename a snapshot (or a branch with `branch_index`) |
| `snap_delete(name, index)` | delete a snapshot |
| `snap_delete_branch(name, index, branch_index)` | delete one branch |

A **branch** is created by *starting from* a snapshot with a new name:
`start(name, snap_index=2, branch_name="experiment")`. This mirrors the GUI,
which has no other way to fork a snapshot.

**Base branches** are branches forked directly off the base disk rather than a
named snapshot — address them with `snap_index = -2`. `snapshots_full(name)`
returns them under a `baseBranches` key (alongside `snapshots`), and you create /
rename / delete them the same way: `start(name, snap_index=-2, branch_name=...)`,
`snap_rename(name, -2, new_name, branch_index=b)`, `snap_delete_branch(name, -2, b)`.
The base itself is **not** a deletable snapshot — `snap_delete(name, -2)` is
refused (deleting the base disk is "delete the VM"); only its branches delete.

### Events (SSE)
`events()` is a generator yielding parsed dicts as the daemon pushes them. It
**blocks** — run it in a thread. Event shapes:

```json
{"event": "vmStateChanged", "name": "dev", "running": true}
{"event": "vmProgress",     "name": "dev", "progress": 42, "staging": false}
{"event": "alert",          "message": "..."}
```

Events are a convenience for live UIs. **For control flow, prefer
`wait()`/`status()`** — they read the current state on each call, so a dropped
connection can never make you miss a transition.

### Waiting
- `wait(name, target_states, timeout=300, interval=2)` — poll until `state` is in
  `target_states` (a string or set); raises `TimeoutError`.
- `wait_online(name, timeout=900)` — shorthand for `wait(name, {"online"})`.

`asb.RUNNING_STATES == {"booting", "online"}` — the powered-on states.

---

## Create config & validation

The daemon validates create/edit input **exactly like the GUI** (it mirrors the
JS guards in `web/app.js`), so the API rejects precisely what the UI would
rather than forwarding bad input to the core.

| Key | Notes |
|---|---|
| `name` | guest hostname; **set at create, not editable later**. Letters/digits/hyphens; not all-digits; no leading/trailing hyphen. Windows ≤15 chars (NetBIOS); Linux ≤63 and lowercase. Must be unique across VMs *and* templates. |
| `osType` | `"Linux"` or `"Windows"` |
| `imagePath` | path to an install ISO — **required unless** `templateName` is given |
| `templateName` | create from a Windows template instead of an ISO |
| `ramMb` | ≥512, **2 MB-aligned** (even). The SDK rounds down to even for you. |
| `hddGb` | ≥1 |
| `cpuCores` | ≥1 |
| `gpuMode` | `0` None · `1` Default (paravirtual) · `2` Try all |
| `networkMode` | `0` None · `1` NAT · `2` External · `3` Internal |
| `netAdapter` | host adapter name (for External mode) |
| `adminUser` | required for a normal create. Linux: ≤32, starts `[a-z_]`, body `[a-z0-9_-]`. Windows: ≤20, none of `"\/[]:;|=,+*?<>`, no trailing `.`, not a reserved name (CON, PRN, …). |
| `adminPass` | required on Linux (≤255 bytes); optional on Windows |
| `testMode` | skip interactive setup where supported |
| `sshEnabled` | provision SSH + forward a loopback port |
| `sshDeployKey` | deploy the AppSandbox public key for password-less login (**requires `sshEnabled`**; rejected `400` otherwise) |
| `isTemplate` | build a template (Windows only; can't be built from another template) |

`edit()` accepts `ramMb`, `cpuCores`, `gpuMode`, `networkMode` with the same
range rules, and **only while the VM is stopped**. `name` cannot be changed.

### SSH key deploy (password-less login)

With `sshEnabled=True, sshDeployKey=True`, the daemon generates an **AppSandbox
ed25519 keypair** once (under `<support dir>/ssh/` — `%ProgramData%\AppSandbox`
on Windows, `~/Library/Application Support/AppSandbox` on macOS) and the guest agent
writes the public key into the guest's `authorized_keys` (Windows:
`administrators_authorized_keys` + the strict ACL sshd needs; Linux/macOS:
`~/.ssh/authorized_keys`). The deployed key is saved in the VM's config. Once it
lands, `sshInfo` reports `keyDeployed: true` and `sshState: 4` (ready + key
deployed), and you can connect with no password:

```python
import asb, subprocess
c = asb.connect()
i = c.ssh_info("dev")                       # {'sshState': 4, 'keyDeployed': True, 'port': ..., 'user': ...}
subprocess.run(["ssh", "-i", asb.key_path(), "-p", str(i["port"]),
                "%s@127.0.0.1" % i["user"], "hostname"])
```

`asb.key_path()` returns the private key path. (`sshState` values: 1 installing,
2 ready, 3 failed, 4 ready + key deployed.)

---

## HTTP status / error model

| Status | When |
|---|---|
| `200 OK` | a query, or a successful `edit` |
| `202 Accepted` | an async action accepted (create/start/stop/snapshot/…) — watch `status`/`events` for the outcome |
| `400 Bad Request` | input failed validation — body has `{"error":{"code":"invalid_arg","message":…}}` |
| `401 Unauthorized` | missing/bad bearer token (only `version` is open) |
| `404 Not Found` | unknown VM/route |
| `405 Method Not Allowed` | wrong verb for the route |
| `409 Conflict` | state conflict: edit/snapshot while running (`vm_running`), delete while building (`vm_building`), daemon shutdown with active VMs (`vms_active`, lists them) |
| `500 Internal Server Error` | the core returned a failure (`error.hr` on Windows, `error.rc` on macOS) |
| `501 Not Implemented` | the feature doesn't exist on this host platform (`not_supported`) — snapshots/templates on macOS; mirror of `capabilities` |

---

## Examples

Runnable scripts in [`examples/`](examples):

| Script | Shows |
|---|---|
| `list_vms.py` | connect, `version`, `host`, `list`, `templates` (read-only overview) |
| `start_vm.py` | `start` + poll to `online` |
| `stop_vm.py` | graceful `shutdown` vs `--force` `stop` |
| `full_lifecycle.py` | the whole surface: create → wait → `ssh_info` → snapshot → edit → branch → delete-branch → delete (self-contained) |
| `stream_events.py` | live SSE event stream |
| `admin.py` | operator/maintenance: `version`, `host`, `templates`, `delete_template`, `shutdown_daemon` (destructive actions gated behind flags) |

---

## Tests

The test suite lives in [`tests/`](tests/) and is **self-contained**: it assumes
no existing configuration, creates the throwaway `brk-*` VMs it needs, and
deletes them at the end (so it never leaves disk behind or touches your VMs).

Point it at your install ISOs first (env vars; or drop the ISOs in `tests/`,
which is the default), then run the harness:

```
set ASB_ISO_LINUX=C:\path\to\ubuntu-desktop-amd64.iso
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

`run_all.py` runs the no-VM static checks once (`test_host_and_validation.py`:
host reporting vs the real machine + create-validation rejections), then spawns
the full per-VM lifecycle (`vm_lifecycle.py`) **concurrently** across specs —
Linux and Windows at the same time — so multi-VM concurrency and every code path
are proven on both OSes simultaneously. The per-VM lifecycle also covers SSH key
auto-deploy: each VM is built with `sshDeployKey` and, once online, the suite
asserts `keyDeployed`/`sshState 4` and performs a key-only SSH login (no
password). `test_windows_template.py` covers the Windows template build →
create-from-template → delete cycle. Default guest credentials are
`user` / `test123`. See [`tests/README.md`](tests/README.md) for the file-by-file
layout.
