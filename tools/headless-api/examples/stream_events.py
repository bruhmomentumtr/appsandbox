"""
Stream daemon events (Server-Sent Events) and print them as they arrive. The
daemon pushes a small JSON object per event (vmStateChanged, agentOnline, build
progress, etc.). This blocks forever -- Ctrl-C to stop.

    python examples/stream_events.py

Events are a convenience for live UIs. For control flow, poll status()/wait():
they read the current state each call, so a dropped connection never loses a
transition.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

c = asb.connect()
print("streaming events (Ctrl-C to stop)...")
try:
    for ev in c.events():
        name = ev.get("name", "")
        kind = ev.get("event", ev.get("raw", "?"))
        print("  %-18s %-14s %s" % (kind, name, {k: v for k, v in ev.items()
                                                 if k not in ("event", "name")}))
except KeyboardInterrupt:
    pass
