"""
Operator / maintenance helpers: inspect the daemon + host, list/delete templates,
and stop the daemon. The destructive actions are gated behind flags, so running
this with no arguments is read-only.

    python examples/admin.py                          # version, host, templates (read-only)
    python examples/admin.py --delete-template NAME   # delete the template named NAME
    python examples/admin.py --shutdown                # stop the daemon (refused if VMs active)
    python examples/admin.py --shutdown --force        # stop the daemon, terminating active VMs
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

c = asb.connect()

v = c.version()
print("daemon %s %s (api %s)" % (v.get("product"), v.get("version"), v.get("apiVersion")))
h = c.host()
print("host: %d cores, %d MB RAM, %d GB free" % (h["hostCores"], h["hostRamMb"], h["freeGb"]))
print("templates:", [t["name"] for t in c.templates()] or "(none)")

if "--delete-template" in sys.argv:
    name = sys.argv[sys.argv.index("--delete-template") + 1]
    print("delete template %r ->" % name, c.delete_template(name))

if "--shutdown" in sys.argv:
    code, body = c.shutdown_daemon(force="--force" in sys.argv)
    if code == 409:
        # refused: VMs are running or building. Re-run with --force to terminate them.
        print("daemon refused shutdown -- active:", body.get("error", {}).get("active"))
    else:
        print("daemon shutdown ->", code, body)
