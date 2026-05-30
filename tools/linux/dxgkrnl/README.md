# dxgkrnl — Hyper-V GPU-PV kernel module for Linux guests

Vendored from microsoft/WSL2-Linux-Kernel branch `linux-msft-wsl-6.18.y`,
`drivers/hv/dxgkrnl/`. Distributed as a DKMS source package so it
rebuilds automatically on kernel upgrades.

## Layout

```
tools/dxgkrnl/
├── README.md
├── Makefile                          ← dev convenience: out-of-tree build for $(uname -r)
├── src/                              ← DKMS package contents (gets copied to /usr/src/dxgkrnl-2.0.3/)
│   ├── dkms.conf
│   ├── Kbuild
│   ├── dxgkrnl_compat.h              ← out-of-tree compat shims (vmbus device-ID GUIDs)
│   ├── dxgmodule.c, dxgadapter.c, ioctl.c, dxgvmbus.c, ...
│   └── include/uapi/misc/d3dkmthk.h  ← from kernel uapi, vendored
└── prebuilt/                         ← cache of binaries known to load on specific kernels
    └── <kernel-vermagic>/dxgkrnl.ko
```

## Install on a Linux VM

The Linux VM-creation flow (TBD) stages `src/` as `/usr/src/dxgkrnl-2.0.3/`
and runs:

```
dkms add  -m dxgkrnl -v 2.0.3
dkms build -m dxgkrnl -v 2.0.3
dkms install -m dxgkrnl -v 2.0.3
echo dxgkrnl > /etc/modules-load.d/dxgkrnl.conf
```

DKMS handles kernel-upgrade survival automatically via the kernel's
postinst `dkms autoinstall` hook.

## Why prebuilt/?

A backup of binaries that we know work. Each entry is keyed by
`uname -r` (e.g. `7.0.0-14-generic`). Useful when DKMS rebuild fails
for any reason (rare). The signed `.ko.zst` files were signed with
the VM's local MOK key — fine because AppSandbox Linux VMs run with
`test_mode=true` (Secure Boot off), so signature verification is not
enforced. They will load on any host's Ubuntu 26.04 / kernel
7.0.0-14-generic.

## Adding support for a new kernel

1. Build a VM with that kernel installed.
2. Stage `src/` as `/usr/src/dxgkrnl-2.0.3/` and run the DKMS dance above.
3. Copy `/lib/modules/$(uname -r)/updates/dkms/dxgkrnl.ko.zst` and its
   decompressed form back to `tools/dxgkrnl/prebuilt/<uname -r>/`.
4. Commit.

## Source origin

`src/*.c`, `src/*.h`, and `src/include/uapi/misc/d3dkmthk.h` are
verbatim from microsoft/WSL2-Linux-Kernel (sparse-checkout). The
upstream in-tree `Makefile` was dropped — out-of-tree builds use
`src/Kbuild` instead. `src/dxgkrnl_compat.h` and `src/dkms.conf` are
new files local to this repo.
