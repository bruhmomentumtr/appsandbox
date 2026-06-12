"""Start a VM and poll until it is up.  Usage: python start_vm.py <name>"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

if len(sys.argv) < 2:
    sys.exit("usage: python start_vm.py <name>   (run list_vms.py to see names)")
name = sys.argv[1]
c = asb.connect()

code, body = c.start(name)
print("start ->", code, body)

print("polling status (Ctrl-C to stop watching)...")
# These VMs can take 10-15 min to reach 'online' (agent connected). Polling is
# miss-proof: the current state is read live each time, so disconnects don't matter.
last = None
import time
while True:
    s = c.status(name)
    if (s["state"], s.get("progress")) != last:
        last = (s["state"], s.get("progress"))
        print(f'  state={s["state"]:10} running={s["running"]} progress={s.get("progress")}')
    if s["state"] == "online":
        print("VM is online (guest agent connected).")
        break
    if s["state"] == "stopped":
        print("VM is stopped (start may have failed -- check the daemon log / alerts).")
        break
    time.sleep(3)
