/*
 * d3dlayers.h -- Host-side fetch of the Microsoft D3D mapping layers.
 *
 * Downloads the Microsoft.D3DMappingLayers package ("OpenCL, OpenGL, and
 * Vulkan Compatibility Pack") directly from Microsoft's servers and extracts
 * the redistributable mapping-layer DLLs into a host cache directory, so they
 * can be staged into a Windows guest (see disk_util.c's manifest generation).
 *
 * This runs on the HOST at VM-create time; the guest never needs internet.
 *
 * The extracted set (in the returned directory) is:
 *   OpenGLOn12.dll      GL  -> D3D12 ICD (the GL shim's target)
 *   dxil.dll            shader validation, required by all layers
 *   OpenCLOn12.dll      OpenCL -> D3D12
 *   clon12compiler.dll  OpenCL compiler backend
 *   vulkan_dzn.dll      Vulkan (Mesa Dozen) -> D3D12
 *   dzn_icd.x64.json    Vulkan ICD manifest (points at vulkan_dzn.dll)
 */

#ifndef D3DLAYERS_H
#define D3DLAYERS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ensure the mapping-layer files are present in a host cache directory.
 *
 * On success returns TRUE and writes the absolute cache directory path
 * (containing the files listed above) into out_dir. The result is cached
 * across calls: if the files are already present no download occurs.
 *
 * Returns FALSE on any failure (no network, Microsoft service error,
 * extraction failure). Callers treat GL/CL/Vulkan acceleration as
 * best-effort and continue VM creation on failure.
 */
BOOL d3dlayers_ensure_cached(wchar_t *out_dir, int out_max);

#ifdef __cplusplus
}
#endif

#endif /* D3DLAYERS_H */
