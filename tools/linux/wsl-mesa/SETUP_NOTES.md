# GPU-PV userspace setup

End-to-end recipe for getting GPU-accelerated 3D apps running on an
AppSandbox Linux VM with a Hyper-V GPU partition attached.

## Architecture

- **Compositor (mutter on Wayland)** stays on **llvmpipe**, rendering
  into asb_drm shmem buffers.
- **Apps that want GPU acceleration** select Mesa's d3d12 gallium
  driver via env vars. Their GL/Vulkan work goes `Mesa → libd3d12.so →
  /dev/dxg → vmbus → host GPU`. Present from app to compositor is
  CPU-mediated (D3D12 CopyResource → READBACK heap → mmap → wl_shm →
  mutter composite). One CPU readback per frame; the rendering work
  itself runs on the host GPU.
- **No kernel changes.** No asb_drm changes. No Mesa source patches.
  Just the custom Mesa build that exposes d3d12 + dzn drivers.

The compositor stays on the CPU and per-app GPU work is routed via the
d3d12 driver.

## Pre-reqs in the VM

`/dev/dxg` must exist (the VM was created with `gpu_mode != GPU_NONE`).
`appsandbox-agent` must have mounted the host's GPU 9P shares at
`/usr/lib/wsl/lib` and `/usr/lib/wsl/drivers/*`.

Sanity check on the live VM:
- `ls /dev/dxg` → present
- `lsmod | grep dxgkrnl` → loaded
- `mount | grep wsl` → 4 shares mounted (Drv.0/1/2 + HostLxssLib)
- `ls /opt/appsandbox/wsl-deps` → contains libd3d12.so, libd3d12core.so,
  libdxcore.so (NuGet-fetched build artifacts shipped via cidata)

## Building Mesa

### deb-src must be enabled

Ubuntu 26.04 (resolute) ships with deb-src disabled. `apt build-dep
mesa` fails silently (no Sources to consult). Enable it:

```
sudo sed -i '/^Types: deb$/c\Types: deb deb-src' /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get update
```

This only matters for the build-host VM. Production VMs install the
prebuilt tarball.

### Build invocation

```
chmod +x build-mesa.sh
nohup ./build-mesa.sh > /tmp/build-mesa.log 2>&1 &
```

`apt build-dep mesa` pulls in ~160 packages: full LLVM 21 dev toolchain,
all Xorg/Wayland headers, libdrm-dev, libwayland-dev, libxcb-* dev,
libpciaccess-dev, libelf-dev, glslang-tools, python3-mako, etc.
Resident set ~700 MB. Then `git clone --depth 1 -b 25.3` of Mesa
(~150 MB checkout) and `meson setup --prefix=/opt/wsl-mesa
--buildtype=release --strip ...` and `ninja -C build -j $(nproc)`.

Meson flags:
```
-D gallium-drivers=llvmpipe,d3d12
-D vulkan-drivers=swrast,microsoft-experimental
-D microsoft-clc=enabled
-D video-codecs=all
-D glvnd=enabled
-D platforms=x11,wayland
```

`microsoft-experimental` is the dzn (Vulkan-on-D3D12) Vulkan driver.
`d3d12` is the gallium driver that gives us GL via libd3d12. `swrast`
is kept so the Vulkan ICD can fall back to lavapipe if needed.
