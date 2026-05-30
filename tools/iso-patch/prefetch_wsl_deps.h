/* prefetch_wsl_deps.h - host-side fetch of Microsoft's WSL D3D12 +
 * DXCore .so libs from their public Azure DevOps NuGet feed.
 *
 * Pure Win32 C: WinHTTP for HTTPS, tar.exe shell-out for .nupkg
 * (zip) extraction. Replaces the manual PowerShell recipe in
 * tools/wsl-deps/README.md so the install doesn't require PS to be
 * enabled on the host.
 */
#ifndef PREFETCH_WSL_DEPS_H
#define PREFETCH_WSL_DEPS_H

#include <windows.h>

/* Download + extract Microsoft.Direct3D.Linux + Microsoft.DXCore.Linux
 * NuGet packages, lay the three .so files directly under <out_dir>:
 *
 *   <out_dir>\
 *     libd3d12.so
 *     libd3d12core.so
 *     libdxcore.so                  (lowercased from libDXCore.so)
 *
 * No host cache — per-VM staging only. Re-downloads every VM-create.
 * Returns 0 on success; non-zero on failure. */
int do_prefetch_wsl_deps(const wchar_t *out_dir);

#endif
