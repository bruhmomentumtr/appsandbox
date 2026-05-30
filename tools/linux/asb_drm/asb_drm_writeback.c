// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — writeback connector (stub).
 *
 * Writeback lets userspace ask the kernel to composite primary + cursor +
 * overlay into a destination buffer in a single atomic commit, so the
 * userland daemon can read one buffer instead of walking each plane.
 *
 * Implementing this correctly requires the driver to perform software
 * blending (alpha-composite the cursor plane onto the primary plane
 * into the userspace-provided destination fb, then signal a fence).
 * That's a chunk of code on top of what we need for Day-1 functionality.
 *
 * For now this returns -ENOSYS so the main probe path logs "writeback
 * init failed, continuing without" and proceeds. The userland daemon
 * already supports the plane-walk fallback path. Writeback lands in a
 * follow-up commit.
 *
 * When implemented, the shape is:
 *   1. drm_writeback_connector_init() with our prepare/cleanup hooks.
 *   2. Hook into asb_crtc_atomic_flush(): if the writeback connector has
 *      a new fb attached, mmap it, mmap the primary + cursor fbs,
 *      memcpy primary → wb, then alpha-blend cursor → wb at the cursor's
 *      crtc_x/crtc_y, then drm_writeback_signal_completion().
 *   3. Use drm_writeback_connector_attach_fence_property() so userspace
 *      gets a sync_file fd it can wait on.
 */

#include "asb_drm.h"

int asb_writeback_init(struct asb_device *asb)
{
	(void)asb;
	return -ENOSYS;
}
