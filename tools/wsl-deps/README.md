# wsl-deps — Microsoft's Linux D3D12/DXCore runtime libs

The Linux GPU-PV userspace stack (Mesa's `d3d12` gallium driver, the
`microsoft-experimental` aka `dzn` Vulkan ICD) calls into Microsoft's
proprietary userspace D3D12 runtime on Linux — three shared libraries:

| File              | What it does                                                                 |
|-------------------|------------------------------------------------------------------------------|
| `libd3d12.so`     | D3D12 API surface; talks to `/dev/dxg` via D3DKMT ioctls                     |
| `libd3d12core.so` | Internal core ABI that `libd3d12.so` links against                           |
| `libdxcore.so`    | Adapter enumeration on Linux (the Linux counterpart of `dxcore.dll`)         |

These are NOT in any public source repo. They are Microsoft-built binary
artifacts that ship as **NuGet packages** on Microsoft's public Azure
DevOps feed — the same one the open-source [`microsoft/WSL`](https://github.com/microsoft/WSL)
repo restores from when building its MSI installer.

## Source of truth

NuGet v3 service index (anonymous, no MSA login required):

```
https://pkgs.dev.azure.com/shine-oss/wsl/_packaging/WslDependencies/nuget/v3/index.json
```

The flat-container endpoint (where the actual `.nupkg` blobs live) is:

```
https://pkgs.dev.azure.com/shine-oss/13eb32df-d33f-470f-b930-499535a958b4/_packaging/7925a3a1-b93c-4977-8a97-5b877bf2068b/nuget/v3/flat2/
```

Two packages, both authored & owned by **Microsoft**, copyright Microsoft:

| Package id                              | Version pinned (currently the only published version) |
|-----------------------------------------|------------------------------------------------------|
| `Microsoft.Direct3D.Linux`              | `1.611.1-81528511`                                   |
| `Microsoft.DXCore.Linux.amd64fre`       | `10.0.26100.1-240331-1435.ge-release`                |

(There is also `Microsoft.DXCore.Linux.arm64fre` with the same version
when we want to support arm64 guests.)

## Where the host installs them

The WSL MSI restores these two NuGet packages and lays them down at
`%SystemRoot%\System32\lxss\lib\` on the Windows host. WSL2 itself
bind-mounts that directory into the guest at `/usr/lib/wsl/lib/` via
9P — which is exactly what our `HostLxssLib` Plan9 share does for our
Linux VMs (see `src/backend_win/gpu_enum.c::gpu_append_lxsslib_share`).

Mesa's `d3d12` gallium driver `dlopen`s `libd3d12.so` at runtime from
that path. Consumers of the driver never need to bundle `libd3d12.so`
themselves — the host-installed copy is the single source of truth.

## What we do with them

At VM-create time (one-time per host install) we run the prefetch
script. It downloads the two `.nupkg` blobs from the flat2 endpoint,
unzips, and lays them out under `C:\ProgramData\AppSandbox\wsl-deps\`:

```
C:\ProgramData\AppSandbox\wsl-deps\
├── current\lib\                                   ← Plan9-shared into the guest
│   ├── libd3d12.so
│   ├── libd3d12core.so
│   └── libdxcore.so                               (renamed from libDXCore.so)
├── direct3d-linux-1.611.1-81528511\lib\
│   ├── x64\{libd3d12.so, libd3d12core.so}
│   └── arm64\{libd3d12.so, libd3d12core.so}
└── dxcore-linux-amd64fre-10.0.26100.1-240331-1435.ge-release\lib\
    └── libdxcore.so
```

`current\lib` is what `gpu_append_lxsslib_share` Plan9-shares into the
guest as `/usr/lib/wsl/lib/`. The versioned dirs are kept alongside for
auditability — if we ever bump the version we can roll back without
re-downloading.

## Hashes for verification

```
b3d78d409a4dbbe8612551fc0c0d746d3e58d7997ee7eba78ce1064d77cfa8c3  x64/libd3d12.so
a4104a2022932d8e6c714f103ebd89db1c5bdbf36fe92151c88d3d93d4e3894d  x64/libd3d12core.so
83d1671a839bcf71709349e77cd68341515df2e63080765618876996a0f4190c  libdxcore.so
```

## Why we don't redistribute them ourselves

These are proprietary Microsoft binaries — we have no license to ship
them in our repo. But Microsoft publishes them on a public NuGet feed
specifically so downstream consumers can fetch them. That's what we do:
prefetch from Microsoft's server on first run, cache, and don't bundle.
Mirrors the way `apt install` fetches Mesa from Ubuntu's server.

## Manual fetch recipe

For reference / disaster recovery, equivalent to whatever
`fetch-wsl-deps.ps1` will end up doing:

```powershell
$feed = 'https://pkgs.dev.azure.com/shine-oss/13eb32df-d33f-470f-b930-499535a958b4/_packaging/7925a3a1-b93c-4977-8a97-5b877bf2068b/nuget/v3/flat2'
$d3dVer    = '1.611.1-81528511'
$dxcoreVer = '10.0.26100.1-240331-1435.ge-release'

Invoke-WebRequest "$feed/microsoft.direct3d.linux/$d3dVer/microsoft.direct3d.linux.$d3dVer.nupkg" `
    -OutFile direct3d.nupkg
Invoke-WebRequest "$feed/microsoft.dxcore.linux.amd64fre/$dxcoreVer/microsoft.dxcore.linux.amd64fre.$dxcoreVer.nupkg" `
    -OutFile dxcore.nupkg

# .nupkg files are zips
Expand-Archive direct3d.nupkg -DestinationPath direct3d
Expand-Archive dxcore.nupkg   -DestinationPath dxcore

# Libs are at:
#   direct3d\build\native\lib\x64\libd3d12.so
#   direct3d\build\native\lib\x64\libd3d12core.so
#   dxcore\build\native\lib\libDXCore.so       (rename to libdxcore.so)
```
