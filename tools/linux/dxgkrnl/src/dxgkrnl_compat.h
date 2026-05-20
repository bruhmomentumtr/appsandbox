/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Out-of-tree compat shims for building dxgkrnl against a vanilla
 * upstream kernel that hasn't taken the two GPU-PV vmbus device-ID
 * macros WSL2-Linux-Kernel adds to <linux/hyperv.h>.
 *
 * These GUIDs identify the global DXGK vmbus channel and the per-vGPU
 * DXGK vmbus channel that the Hyper-V host exposes when GPU
 * paravirtualisation is enabled on a VM. dxgkrnl matches against them
 * in its hv_vmbus_device_id table.
 *
 * Force-included via -include $(src)/dxgkrnl_compat.h in src/Kbuild.
 */

#ifndef _DXGKRNL_COMPAT_H
#define _DXGKRNL_COMPAT_H

#include <linux/hyperv.h>

#ifndef HV_GPUP_DXGK_GLOBAL_GUID
/* {DDE9CBC0-5060-4436-9448-EA1254A5D177} */
#define HV_GPUP_DXGK_GLOBAL_GUID \
	.guid = GUID_INIT(0xdde9cbc0, 0x5060, 0x4436, 0x94, 0x48, \
			  0xea, 0x12, 0x54, 0xa5, 0xd1, 0x77)
#endif

#ifndef HV_GPUP_DXGK_VGPU_GUID
/* {6E382D18-3336-4F4B-ACC4-2B7703D4DF4A} */
#define HV_GPUP_DXGK_VGPU_GUID \
	.guid = GUID_INIT(0x6e382d18, 0x3336, 0x4f4b, 0xac, 0xc4, \
			  0x2b, 0x77, 0x3, 0xd4, 0xdf, 0x4a)
#endif

#endif /* _DXGKRNL_COMPAT_H */
