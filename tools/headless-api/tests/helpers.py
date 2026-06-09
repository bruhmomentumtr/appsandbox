"""
Shared helpers for the headless-api test suite. Every test prepares the throwaway
VM(s) it needs and removes them afterward -- no test assumes any pre-existing VM
or configuration. Throwaways are named ``brk-<tag>``; ``destroy_vm`` removes both
the VM and its on-disk folder.

Install ISOs default to this folder (drop the ISOs next to the tests), or override
with env vars: ``ASB_ISO_LINUX`` and ``ASB_ISO_WINDOWS``.
"""
import os, sys, time, shutil, subprocess, tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # for asb.py (parent dir)
import asb

_HERE = os.path.dirname(os.path.abspath(__file__))
ASB_DIR = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "AppSandbox")
ISO_LIN = os.environ.get("ASB_ISO_LINUX",   os.path.join(_HERE, "ubuntu-desktop-amd64.iso"))
ISO_WIN = os.environ.get("ASB_ISO_WINDOWS", os.path.join(_HERE, "Win11_x64.iso"))


def status_or_none(c, name):
    """Status dict for ``name``, or None if the VM no longer exists (404)."""
    try:
        return c.status(name)
    except KeyError:
        return None


def destroy_vm(c, name):
    """Force-stop, delete, and rmtree a throwaway VM -- guard-aware (waits out any
    in-flight build, since delete is refused while building). Safe to call on a
    name that no longer exists."""
    deadline = time.time() + 600
    while time.time() < deadline:
        s = status_or_none(c, name)
        if s is None:
            break
        if s["building"]:
            time.sleep(5); continue
        if s["running"]:
            c.stop(name); time.sleep(3)
        c.delete_vm(name); time.sleep(2)
        break
    d = os.path.join(ASB_DIR, name)
    if os.path.isdir(d):
        shutil.rmtree(d, ignore_errors=True)


def ssh_key_login(port, user, command="whoami", timeout=90):
    """Run ``command`` in the guest over SSH using the deployed AppSandbox key,
    with BatchMode (key auth only -- no password fallback, so success proves the
    key works). The host-side private key in %ProgramData% is admin-owned and ssh
    refuses broad-permission keys, so it is copied to a user-private temp file
    first. Returns ``(returncode, stdout, stderr)``."""
    keysrc = asb.key_path()
    if not os.path.isfile(keysrc):
        return (1, "", "AppSandbox private key not found: %s" % keysrc)
    tmpkey = os.path.join(tempfile.gettempdir(), "asb_test_id")
    known = os.path.join(tempfile.gettempdir(), "asb_known_hosts")
    shutil.copyfile(keysrc, tmpkey)
    subprocess.run(["icacls", tmpkey, "/inheritance:r", "/grant:r",
                    "%s:F" % os.environ.get("USERNAME", "")], capture_output=True)
    try:
        r = subprocess.run(
            ["ssh", "-i", tmpkey, "-p", str(port),
             "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=" + known,
             "-o", "BatchMode=yes", "-o", "ConnectTimeout=20",
             "%s@127.0.0.1" % user, command],
            capture_output=True, text=True, timeout=timeout)
        return (r.returncode, r.stdout, r.stderr)
    except Exception as e:
        return (1, "", repr(e))
    finally:
        for junk in (tmpkey, known):
            try: os.remove(junk)
            except OSError: pass
