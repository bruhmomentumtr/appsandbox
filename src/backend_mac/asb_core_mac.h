/*
 * asb_core_mac.h -- macOS orchestrator public API.
 *
 * Mirrors asb_core.h on Windows: owns the in-memory VM array, INI-style
 * persistence (vms.cfg), lifecycle (create/start/stop/delete), and config
 * editing. ui.m calls these functions directly.
 */

#ifndef ASB_CORE_MAC_H
#define ASB_CORE_MAC_H

#import <Foundation/Foundation.h>

@class VzVm, VzDisplayWindow, VmAgentMac, VmSshProxyMac;

#define ASB_MAX_VMS 32

typedef struct {
    char    name[256];
    char    os_type[32];
    char    admin_user[64];
    char    admin_pass[128];
    int     ram_mb;
    int     hdd_gb;
    int     cpu_cores;
    int     gpu_mode;
    int     network_mode;
    BOOL    running;
    BOOL    shutting_down;
    BOOL    install_complete;
    BOOL    agent_online;
    uint64_t agent_last_heartbeat_ms;
    BOOL    ssh_enabled;            /* user-configured at create time */
    int     ssh_port;               /* host loopback port, 0 = unassigned */
    int     ssh_state;              /* 0=off 1=installing 2=ready 3=failed
                                       (reported as 4 when ready && ssh_key_deployed) */
    BOOL    ssh_deploy_key;         /* TRUE = deploy the AppSandbox public key to the guest */
    BOOL    ssh_key_deployed;       /* TRUE once the guest agent has written authorized_keys
                                       (volatile: reset on stop -- re-deployed each boot) */
    char    ssh_pubkey[512];        /* AppSandbox public-key line to deploy (ed25519) */
    int     install_progress;
    char    install_status[128];
    VzVm            *__unsafe_unretained vz_handle;
    VzDisplayWindow *__unsafe_unretained display;
    VmAgentMac      *__unsafe_unretained agent;
    VmSshProxyMac   *__unsafe_unretained ssh_proxy;
} AsbVmMac;

void asb_mac_init(void);
void asb_mac_cleanup(void);

int          asb_mac_vm_count(void);
AsbVmMac    *asb_mac_vm_get(int index);
AsbVmMac    *asb_mac_vm_find(const char *name);

int  asb_mac_vm_create(const char *name, const char *os_type,
                        int ram_mb, int hdd_gb, int cpu_cores,
                        int gpu_mode, int network_mode,
                        const char *image_path,
                        const char *admin_user,
                        const char *admin_pass,
                        BOOL ssh_enabled,
                        BOOL ssh_deploy_key);
int  asb_mac_vm_start(const char *name);
int  asb_mac_vm_stop(const char *name, int force);
int  asb_mac_vm_delete(const char *name);
int  asb_mac_vm_edit(const char *name, const char *field, const char *value);

void asb_mac_save(void);

/* Send a mute/unmute command to the VM's guest agent. Called from the
 * display window's open/close hooks so the guest stops/starts driving
 * audio when nobody is watching. No-op if the agent isn't online. */
void asb_mac_vm_set_audio_muted(const char *name, BOOL muted);

/* Toggle per-VM clipboard syncing. The display window flips this on
 * becomeKey / resignKey so host clipboard data is only shared with the
 * guest while the user is actually using the VM — avoids background
 * leakage when the user is working in other apps on the host. */
void asb_mac_vm_set_clipboard_sync(const char *name, BOOL enabled);

typedef void (*AsbMacEventCallback)(int type, const char *vm_name,
                                     int int_value, const char *str_value);
void asb_mac_set_event_cb(AsbMacEventCallback cb);

/* Headless mode -- call BEFORE asb_mac_init / any VM start. Gates the parts of
 * the core that need a GUI login session:
 *   - the per-VM NSWindow + VZVirtualMachineView created on the Running
 *     transition (the one window-server dependency in the VM path);
 *   - the clipboard channel (NSPasteboard is per-Aqua-session, and the host
 *     poll/serve machinery must not run in a daemon);
 *   - the VM's audio devices (host microphone capture would hang on a TCC
 *     prompt no daemon can show, and output would play on the host speakers).
 * Display/clipboard teardown paths are nil-guarded no-ops. Mirrors Windows,
 * where the display/clipboard layers live in the GUI app and simply never
 * activate under --headless. */
void asb_mac_set_headless(BOOL headless);

/* Path of the AppSandbox SSH private key (~/Library/Application Support/
 * AppSandbox/ssh/id_appsandbox); pair .pub is deployed into guests created
 * with ssh_deploy_key. Returns the path whether or not the key exists yet. */
NSString *asb_mac_ssh_key_path(void);

#endif
