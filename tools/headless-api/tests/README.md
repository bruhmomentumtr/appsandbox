# headless-api tests

Self-contained integration tests for the AppSandbox headless daemon. They drive
the daemon over its HTTP/JSON API through the [`asb.py`](../asb.py) client — no
test framework, just stdlib Python. Every test creates the throwaway VMs it needs
(named `brk-*`) and deletes them at the end, so the suite **assumes no existing
configuration and never touches your VMs, templates, or snapshots**.

## Running

Start the daemon (elevated), point the suite at your install ISOs, then run the
harness from this folder's parent:

```
appsandbox.exe --headless
set ASB_ISO_LINUX=C:\path\to\ubuntu-desktop-amd64.iso
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

ISOs default to this `tests/` folder if the env vars are unset (drop them in
here and the defaults pick them up). A full run builds real Linux and Windows
guests concurrently — it is long (Windows install + sysprep can take 20-30 min)
and uses real disk, so make sure there is free space before starting.

## Files

| File | What it covers |
| --- | --- |
| `run_all.py` | Master harness. Runs the static checks once, then one full per-VM lifecycle per spec **concurrently** (Linux + Windows), then the Windows template lifecycle. Prints a per-thread summary and a coverage table (endpoint → where tested). This is the entry point. |
| `vm_lifecycle.py` | The full per-VM sequence, parametrized by a create-spec. Covers create / status / edit / start / graceful shutdown / force-stop / snapshots + branches / base branches / SSE / delete, plus the build-time guards and **SSH key auto-deploy** (build with `sshDeployKey`, assert `keyDeployed`/`sshState 4`, then a key-only login). `run_all.py` runs this once per OS, in parallel. |
| `test_host_and_validation.py` | No-VM checks: host cores/RAM cross-checked against the exact Win32 calls the daemon makes, and the create-input-validation rejections (matching the GUI's JS guards). Builds nothing. |
| `test_windows_template.py` | Windows template lifecycle: create a template (`isTemplate`), wait for sysprep finalization (moves from `/vms` to `/templates`), create a VM from it, then delete the template. |
| `helpers.py` | Shared helpers: ISO/ProgramData paths, `destroy_vm` (build-guard-aware teardown), and `ssh_key_login` (key-only BatchMode SSH login used by the key-deploy check). |

## Notes

- **Lifecycle discipline mirrors the GUI.** Every VM start boots all the way to
  *online* (guest agent connected) before the VM is used; the only exceptions are
  the delete/edit-during-build guards, which deliberately act before the install
  finishes. Every shutdown is the graceful, agent-mediated one — force-stop is
  exercised in exactly one dedicated test.
- **Linux/macOS SSH key-deploy depends on a pushed branch.** The Linux and macOS
  guest agents are built from the GitHub branch this binary points at, so the
  key-deploy checks stay red on those guests until the `headless-api` branch is
  pushed. The Windows agent is built locally and passes immediately. The login is
  gated on `keyDeployed`, so a non-deployed guest just fails that check rather
  than hanging.
- Default guest credentials are `user` / `test123`.
