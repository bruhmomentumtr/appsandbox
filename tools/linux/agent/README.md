# App Sandbox Linux guest daemons

In-VM Linux daemons that speak the same vsock wire protocol the Windows
App Sandbox host expects. See `docs/linux-idd-implementation-plan.md` in
the repo root for full architecture and wire-protocol reference.

## Daemons (this directory)

| Binary | Channel | Listens on | Privilege | Status |
|---|---|---|---|---|
| `appsandbox-agent` | control | vsock :1 | root (system) | implemented |
| `appsandbox-input` | input | vsock :3 | root (system) | planned |
| `appsandbox-display` | frames + cursor | vsock :2 | root (system) | planned |
| `appsandbox-clipboard` | clipboard | vsock :5, :6 | user (user unit) | planned |

## Build (inside the VM)

```sh
sudo apt-get install -y build-essential
make
```

## Install + enable (inside the VM)

```sh
sudo make enable
```

This installs to `/usr/local/bin/`, drops the systemd unit into
`/etc/systemd/system/`, runs `daemon-reload`, then
`systemctl enable --now appsandbox-agent.service`.

## Smoke test

```sh
systemctl status appsandbox-agent
journalctl -u appsandbox-agent -f
```

You should see `listening on AF_VSOCK port 1`. Once the host-side Phase 1
service-GUID switch lands, the App Sandbox UI's agent dot will turn green
within a few seconds of the unit being active.

## Uninstall

```sh
sudo make disable
sudo rm /usr/local/bin/appsandbox-agent /etc/systemd/system/appsandbox-agent.service
sudo systemctl daemon-reload
```
