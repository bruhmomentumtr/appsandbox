/* ubuntu_vhdx.h - direct Ubuntu ISO -> bootable VHDX (no subiquity).
 *
 * Reads an Ubuntu desktop ISO, extracts casper/minimal.squashfs into a
 * fresh ext4 partition, installs signed shim+grub from the ISO's .debs,
 * plants a one-shot first-boot systemd unit that runs grub-install +
 * update-grub on first boot to byte-converge with vanilla Ubuntu.
 *
 * Output protocol: same STATUS:/PROGRESS:/ERROR:/DONE: stdout/stderr
 * lines as the rest of iso-patch.
 */
#ifndef UBUNTU_VHDX_H
#define UBUNTU_VHDX_H

#include <windows.h>

/* iso_path        : path to ubuntu-XX.XX-desktop-<arch>.iso (amd64 or arm64)
 * vhdx_path       : output VHDX (overwritten if exists)
 * size_gb         : virtual disk size in GiB (>= 16)
 * manifest_path   : NULL or tab-separated <src>\t<rootfs-dest> to inject
 *                   files into the rootfs (analog of --stage for Windows)
 *
 * Returns 0 on success. */
int do_ubuntu_to_vhdx(const wchar_t *iso_path,
                      const wchar_t *vhdx_path,
                      int size_gb,
                      const wchar_t *manifest_path);

#endif
