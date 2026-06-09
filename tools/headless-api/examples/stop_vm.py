"""Stop a VM.  Usage: python stop_vm.py <name> [--force]

Default is a graceful ACPI shutdown (the guest powers itself off); --force
hard-stops it (equivalent to pulling the plug).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

args = [a for a in sys.argv[1:] if not a.startswith("-")]
if not args:
    sys.exit("usage: python stop_vm.py <name> [--force]")
name = args[0]
force = "--force" in sys.argv
c = asb.connect()

if force:
    print("force stop ->", c.stop(name))
    timeout = 60
else:
    print("graceful shutdown ->", c.shutdown(name))
    timeout = 600   # a graceful guest shutdown (esp. Windows) can take minutes

s = c.wait(name, "stopped", timeout=timeout, interval=2)
print("state:", s["state"])
