#ifndef DISK_UTIL_H
#define DISK_UTIL_H

#include <windows.h>

/* Create a new dynamically-expanding VHDX file.
   size_gb: maximum virtual size in gigabytes. */
HRESULT vhdx_create(const wchar_t *path, ULONGLONG size_gb);

/* Create a differencing (child) VHDX that references parent_path.
   New writes go to child_path; parent_path remains unchanged. */
HRESULT vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path);

/* Create a differencing child, then resize it to size_gb GiB. The child
   normally inherits the parent's MaximumSize; on Win10+ ResizeVirtualDisk
   can stretch the differencing VHDX past the parent, giving per-VM disk
   sizing without re-doing the qcow2 → raw → VHDX conversion. If size_gb
   is 0 or <= parent's size, falls through to plain vhdx_create_differencing
   with no resize attempt. */
HRESULT vhdx_create_differencing_resized(const wchar_t *child_path,
                                          const wchar_t *parent_path,
                                          ULONGLONG size_gb);

/* Merge a differencing VHDX into its parent.
   After merge, child_path can be deleted. */
HRESULT vhdx_merge(const wchar_t *child_path);

/* Create a resources ISO containing autounattend.xml, agent, and helper
   executables for unattended Windows install with GPU-PV. GPU driver
   file copy is handled by the agent service's embedded 9P client (p9copy).
   res_dir: directory containing the agent and helper executables.
   The password is wiped from memory after use. */
HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode,
                              BOOL ssh_enabled,
                              const wchar_t *lang);

/* Create a resources ISO for an instance created from a template.
   Contains unattend.xml (not autounattend.xml) for post-sysprep mini-setup
   with the real user account, agent, and GPU copy setup. */
HRESULT iso_create_instance_resources(const wchar_t *iso_path,
                                       const wchar_t *vm_name,
                                       const wchar_t *admin_user,
                                       wchar_t *admin_pass,
                                       const wchar_t *res_dir,
                                       BOOL ssh_enabled,
                                       const wchar_t *lang);

/* Create a cloud-init NoCloud "cidata" ISO for Ubuntu guests. Combines
   the cidata datasource (user-data + meta-data) with the resources tree
   (agent ELFs, dxgkrnl + asb_drm modules, wsl-mesa, wsl-deps .so, setup.sh,
   systemd units) on a single ISO9660 volume labelled "cidata". On a
   freshly-booted Ubuntu Server cloud image, cloud-init reads user-data
   from this ISO and runs setup.sh which installs ubuntu-desktop, our
   agent service, and the GPU-PV bits.
   The password is wiped from memory after use. */
/* `nat_ip` is the host-allocated /24 address (e.g. "192.168.42.5") for
   NAT-mode VMs, or NULL for other network modes (external = DHCP works,
   none = no network). When set, the generated network-config writes a
   static netplan for eth0 so cloud-init brings networking up at boot
   without waiting for DHCP. The agent overrides this on every boot via
   /etc/netplan/99-appsandbox.yaml. */
HRESULT iso_create_resources_ubuntu(const wchar_t *iso_path,
                                     const wchar_t *vm_name,
                                     const wchar_t *admin_user,
                                     wchar_t *admin_pass,
                                     const wchar_t *res_dir,
                                     BOOL ssh_enabled,
                                     const char *nat_ip);

/* Ensure the cached, ready-to-boot Ubuntu cloud-image VHDX exists in
   C:\ProgramData\AppSandbox\linux-base\<version>\base.vhdx. If not,
   downloads the .tar.gz from cloud-images.ubuntu.com, extracts the raw
   .img, appends a VHD-fixed footer, converts VHD->VHDX via
   CreateVirtualDisk SourcePath, marks the result read-only.

   `version` is currently a fixed string: L"ubuntu-26.04". Future
   versions will accept other LTS codenames.

   On success, fills `out_vhdx_path` with the cached file path.
   Returns S_OK if the file is in the cache (either already-cached or
   freshly-downloaded). */
HRESULT ensure_ubuntu_cloud_image_cached(const wchar_t *version,
                                          wchar_t *out_vhdx_path,
                                          int out_max);

/* ---- VHDX-First VM Creation (no resources ISO) ---- */

/* Forward declaration for GPU share list */
struct GpuDriverShareList;

/* Generate unattend.xml for VHDX-first boot (specialize + oobeSystem, no windowsPE).
   Returns TRUE on success. */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode,
                             const wchar_t *lang);

/* Generate unattend.xml for VHDX-first *template* boot.
   Boots into audit mode, runs sysprep /generalize /oobe /shutdown /mode:vm.
   No user account or password needed. */
BOOL generate_unattend_vhdx_template(const wchar_t *output_path,
                                      const wchar_t *vm_name,
                                      BOOL test_mode);

/* Generate setup.cmd for VHDX-first boot (agent already on disk). */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path);

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk). */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path, BOOL ssh_enabled);

/* Generate staging manifest for iso-patch --stage.
   Writes tab-separated source\tdest lines for all files to pre-stage on the VHDX.
   gpu_shares may be NULL if no GPU.
   Returns number of entries written, or -1 on error. */
int generate_vhdx_manifest(const wchar_t *manifest_path,
                            const wchar_t *staging_dir,
                            const wchar_t *res_dir,
                            const void *gpu_shares,
                            BOOL ssh_enabled);

/* Ensure OpenSSH MSI is cached locally (downloads if needed).
   Returns TRUE if the MSI is available at the returned path. */
BOOL ensure_ssh_msi_cached(wchar_t *msi_path_out, int max_chars);

/* Stage the Linux agent ELFs + kernel modules + DKMS sources + systemd
   units + modprobe.d/modules-load.d + wsl-mesa + wsl-deps under
   <staging>/extras/. Same content the cidata builder used to ship —
   exposed so the new direct-ISO->VHDX flow can populate a staging dir
   and turn it into a manifest for iso-patch --stage. */
void stage_linux_agent_and_extras(const wchar_t *staging,
                                  const wchar_t *res_dir,
                                  BOOL ssh_enabled);

/* Write language.json alongside VHDX */
void vm_save_language_json(const wchar_t *vhdx_path, const wchar_t *lang);

/* Read language.json — returns TRUE if found, fills lang_out */
BOOL vm_load_language_json(const wchar_t *vhdx_path, wchar_t *lang_out, int lang_out_max);

#endif /* DISK_UTIL_H */
