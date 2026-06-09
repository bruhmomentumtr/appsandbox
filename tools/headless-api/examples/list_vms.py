"""
Overview: connect to the daemon and print the version, host capacity, the VM
list, and the templates. Read-only -- safe to run anytime.

    python examples/list_vms.py
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

c = asb.connect()                       # reads %ProgramData%\AppSandbox\host.json

v = c.version()
print("daemon: %s %s (api %s)" % (v.get("product"), v.get("version"), v.get("apiVersion")))

h = c.host()
print("host:   %d cores, %d MB RAM, %d GB free  |  VMs using: %d cores, %d MB, %d GB"
      % (h["hostCores"], h["hostRamMb"], h["freeGb"], h["vmCores"], h["vmRamMb"], h["vmHddGb"]))

print("\nVMs:")
for vm in c.list():
    extra = "agent" if vm.get("agentOnline") else ""
    if vm.get("sshState") == 2:
        extra += "  ssh:127.0.0.1:%d" % vm["sshPort"]
    print("  %-16s %-8s %-11s %s" % (vm["name"], vm["osType"], vm["state"], extra))

tpls = c.templates()
print("\ntemplates:", ", ".join("%s (%s)" % (t["name"], t["osType"]) for t in tpls) or "(none)")
