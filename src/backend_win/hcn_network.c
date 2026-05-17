#include <winsock2.h>
#include "hcn_network.h"
#include "ui.h"
#include <stdio.h>
#include <objbase.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

#pragma comment(lib, "ole32.lib")

/* ---- HCN function pointer types ---- */

typedef HRESULT (WINAPI *PFN_HcnCreateNetwork)(
    REFGUID id, PCWSTR settings, void **network, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnCreateEndpoint)(
    void *network, REFGUID id, PCWSTR settings, void **endpoint, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnDeleteNetwork)(
    REFGUID id, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnDeleteEndpoint)(
    REFGUID id, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnCloseNetwork)(void *network);
typedef HRESULT (WINAPI *PFN_HcnCloseEndpoint)(void *endpoint);
typedef HRESULT (WINAPI *PFN_HcnOpenNetwork)(
    REFGUID id, void **network, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnOpenEndpoint)(
    REFGUID id, void **endpoint, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnEnumerateEndpoints)(
    PCWSTR query, PWSTR *endpoints, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnQueryEndpointProperties)(
    void *endpoint, PCWSTR query, PWSTR *properties, PWSTR *errorRecord);

/* ---- Loaded function pointers ---- */

static HMODULE g_hcn_dll = NULL;

static PFN_HcnCreateNetwork           pfnCreateNet;
static PFN_HcnCreateEndpoint          pfnCreateEp;
static PFN_HcnDeleteNetwork           pfnDeleteNet;
static PFN_HcnDeleteEndpoint          pfnDeleteEp;
static PFN_HcnCloseNetwork            pfnCloseNet;
static PFN_HcnCloseEndpoint           pfnCloseEp;
static PFN_HcnOpenNetwork             pfnOpenNet;
static PFN_HcnOpenEndpoint            pfnOpenEp;
static PFN_HcnEnumerateEndpoints      pfnEnumEp;
static PFN_HcnQueryEndpointProperties pfnQueryEpProps;

/* ---- Helpers ---- */

static void guid_to_string(const GUID *g, wchar_t *out, size_t out_len)
{
    swprintf_s(out, out_len,
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ---- Public API ---- */

typedef HRESULT (WINAPI *PFN_HcnEnumerateNetworks)(
    PCWSTR query, PWSTR *networks, PWSTR *errorRecord);

static PFN_HcnEnumerateNetworks pfnEnumNet;

BOOL hcn_init(void)
{
    g_hcn_dll = LoadLibraryW(L"computenetwork.dll");
    if (!g_hcn_dll)
        return FALSE;

    pfnCreateNet  = (PFN_HcnCreateNetwork)GetProcAddress(g_hcn_dll, "HcnCreateNetwork");
    pfnCreateEp   = (PFN_HcnCreateEndpoint)GetProcAddress(g_hcn_dll, "HcnCreateEndpoint");
    pfnDeleteNet  = (PFN_HcnDeleteNetwork)GetProcAddress(g_hcn_dll, "HcnDeleteNetwork");
    pfnDeleteEp   = (PFN_HcnDeleteEndpoint)GetProcAddress(g_hcn_dll, "HcnDeleteEndpoint");
    pfnCloseNet   = (PFN_HcnCloseNetwork)GetProcAddress(g_hcn_dll, "HcnCloseNetwork");
    pfnCloseEp    = (PFN_HcnCloseEndpoint)GetProcAddress(g_hcn_dll, "HcnCloseEndpoint");
    pfnOpenNet    = (PFN_HcnOpenNetwork)GetProcAddress(g_hcn_dll, "HcnOpenNetwork");
    pfnEnumNet    = (PFN_HcnEnumerateNetworks)GetProcAddress(g_hcn_dll, "HcnEnumerateNetworks");
    pfnOpenEp     = (PFN_HcnOpenEndpoint)GetProcAddress(g_hcn_dll, "HcnOpenEndpoint");
    pfnEnumEp     = (PFN_HcnEnumerateEndpoints)GetProcAddress(g_hcn_dll, "HcnEnumerateEndpoints");
    pfnQueryEpProps = (PFN_HcnQueryEndpointProperties)GetProcAddress(g_hcn_dll, "HcnQueryEndpointProperties");

    if (!pfnCreateNet || !pfnCreateEp || !pfnCloseNet || !pfnCloseEp) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
        return FALSE;
    }

    return TRUE;
}

/* Fixed GUIDs for AppSandbox networks so we can clean up across runs */
static const GUID APPSANDBOX_NAT_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }
};

static const GUID APPSANDBOX_INTERNAL_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x77 }
};

static const GUID APPSANDBOX_EXTERNAL_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x88 }
};

/* Find the adapter that carries the default route (0.0.0.0/0). */
/* Find a physical adapter that is UP, has a default gateway, and isn't virtual.
   Prefers Ethernet (IF_TYPE_ETHERNET_CSMACD) over Wi-Fi (IF_TYPE_IEEE80211). */
static BOOL get_default_adapter_name(wchar_t *out, size_t out_len)
{
    ULONG buf_len;
    PIP_ADAPTER_ADDRESSES addrs;
    PIP_ADAPTER_ADDRESSES cur;
    PIP_ADAPTER_ADDRESSES best_eth = NULL;
    PIP_ADAPTER_ADDRESSES best_wifi = NULL;
    PIP_ADAPTER_ADDRESSES pick;
    DWORD ret;

    buf_len = 15000;
    addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
    if (!addrs) return FALSE;

    ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs) return FALSE;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, addrs, &buf_len);
    }
    if (ret != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, addrs);
        return FALSE;
    }

    for (cur = addrs; cur != NULL; cur = cur->Next) {
        BOOL has_gateway = FALSE;

        /* Must be UP */
        if (cur->OperStatus != IfOperStatusUp)
            continue;

        /* Skip virtual adapters (Hyper-V vSwitch, VPN, loopback) */
        if (wcsstr(cur->FriendlyName, L"vEthernet") != NULL)
            continue;
        if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (cur->IfType == IF_TYPE_TUNNEL)
            continue;

        /* Must have a gateway (i.e. connected to a network with internet) */
        if (cur->FirstGatewayAddress != NULL)
            has_gateway = TRUE;
        if (!has_gateway)
            continue;

        /* Prefer Ethernet over Wi-Fi */
        if (cur->IfType == IF_TYPE_ETHERNET_CSMACD && !best_eth)
            best_eth = cur;
        else if (cur->IfType == IF_TYPE_IEEE80211 && !best_wifi)
            best_wifi = cur;
    }

    pick = best_eth ? best_eth : best_wifi;
    if (pick) {
        wcscpy_s(out, out_len, pick->FriendlyName);
        ui_log(L"Selected adapter: %s (type %lu, index %lu)",
               out, pick->IfType, pick->IfIndex);
        HeapFree(GetProcessHeap(), 0, addrs);
        return TRUE;
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return FALSE;
}

/* Check if a network already exists by GUID. Returns TRUE if it does. */
static BOOL hcn_network_exists(const GUID *id)
{
    void *network = NULL;
    PWSTR er = NULL;
    HRESULT hr;

    if (!pfnOpenNet) return FALSE;

    hr = pfnOpenNet(id, &network, &er);
    if (er) LocalFree(er);
    if (SUCCEEDED(hr) && network) {
        if (pfnCloseNet) pfnCloseNet(network);
        return TRUE;
    }
    return FALSE;
}

/* Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into a GUID. */
static BOOL parse_guid_w(const wchar_t *s, GUID *out)
{
    unsigned int d1;
    unsigned int d2, d3;
    unsigned int b[8];
    if (swscanf_s(s, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  &d1, &d2, &d3,
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return FALSE;
    out->Data1 = d1;
    out->Data2 = (USHORT)d2;
    out->Data3 = (USHORT)d3;
    {
        int i;
        for (i = 0; i < 8; i++) out->Data4[i] = (BYTE)b[i];
    }
    return TRUE;
}

/* Returns TRUE if the endpoint at `ep_id` is attached to any of our three
   networks (NAT / INTERNAL / EXTERNAL). Verified server-side via
   HcnQueryEndpointProperties so we never delete somebody else's endpoint. */
static BOOL endpoint_in_our_networks(const GUID *ep_id)
{
    void *ep = NULL;
    PWSTR props = NULL;
    PWSTR er = NULL;
    BOOL match = FALSE;
    wchar_t nat_g[64], int_g[64], ext_g[64];

    if (!pfnOpenEp || !pfnQueryEpProps || !pfnCloseEp) return FALSE;

    if (FAILED(pfnOpenEp(ep_id, &ep, &er)) || !ep) {
        if (er) LocalFree(er);
        return FALSE;
    }
    if (er) { LocalFree(er); er = NULL; }

    if (SUCCEEDED(pfnQueryEpProps(ep, L"{\"SchemaVersion\":{\"Major\":2,\"Minor\":0}}", &props, &er)) && props) {
        guid_to_string(&APPSANDBOX_NAT_GUID,      nat_g, 64);
        guid_to_string(&APPSANDBOX_INTERNAL_GUID, int_g, 64);
        guid_to_string(&APPSANDBOX_EXTERNAL_GUID, ext_g, 64);
        if (wcsstr(props, nat_g) != NULL ||
            wcsstr(props, int_g) != NULL ||
            wcsstr(props, ext_g) != NULL)
            match = TRUE;
    }
    if (props) LocalFree(props);
    if (er) LocalFree(er);
    pfnCloseEp(ep);
    return match;
}

/* Sweep every HCN endpoint whose HostComputeNetwork is one of ours, and
   delete it. Leaves the AppSandbox networks themselves intact so the
   existing vEthernet adapter + subnet are reused across runs.

   Safe at startup: HCS auto-terminates every compute system whose last
   handle is closed (we set ShouldTerminateOnLastHandleClosed=true), so
   by the time we run this no VM is alive to be holding a real endpoint. */
void hcn_cleanup_stale_endpoints(void)
{
    PWSTR all_eps = NULL;
    PWSTR er = NULL;
    int deleted = 0;

    if (!pfnEnumEp || !pfnDeleteEp) return;

    if (FAILED(pfnEnumEp(L"{\"SchemaVersion\":{\"Major\":2,\"Minor\":0}}",
                          &all_eps, &er)) || !all_eps) {
        if (er) LocalFree(er);
        return;
    }
    if (er) { LocalFree(er); er = NULL; }

    /* Walk the returned JSON array of GUID strings. Format is roughly
       ["xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", ...]. We extract each
       36-char run inside double-quotes and try to parse it as a GUID. */
    {
        wchar_t *p = all_eps;
        while (*p) {
            if (*p == L'"') {
                wchar_t buf[40];
                int n = 0;
                p++;
                while (*p && *p != L'"' && n < 39) buf[n++] = *p++;
                buf[n] = L'\0';
                if (n == 36) {
                    GUID ep_id;
                    if (parse_guid_w(buf, &ep_id) &&
                        endpoint_in_our_networks(&ep_id)) {
                        pfnDeleteEp(&ep_id, &er);
                        if (er) { LocalFree(er); er = NULL; }
                        deleted++;
                    }
                }
                if (*p == L'"') p++;
            } else {
                p++;
            }
        }
    }

    LocalFree(all_eps);
    if (deleted > 0)
        ui_log(L"HCN: cleared %d stale endpoint(s) from prior run.", deleted);
}

void hcn_cleanup(void)
{
    if (g_hcn_dll) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
    }
}

/* -------------------------------------------------------------------------
 *  NAT subnet picker
 *
 *  Default Switch (built-in on every Windows 11) lives at 172.20.48.0/20.
 *  Our previous static 172.20.0.0/16 OVERLAPPED that and HCN refused with
 *  ERROR_DUP_NAME (Microsoft re-uses the error code; the message lies).
 *
 *  Strategy: enumerate every IPv4 subnet bound to a host adapter and pick
 *  the first 192.168.N.0/24 with N>=42 that does not overlap any of them.
 *  Cached for process lifetime.
 * ------------------------------------------------------------------------- */

static char g_nat_subnet_prefix[32];   /* "192.168.42.0/24" */
static char g_nat_gateway[32];         /* "192.168.42.1"    */
static char g_nat_ip_base[32];         /* "192.168.42."     */
static int  g_nat_prefix_len;          /* 24                */
static BOOL g_nat_subnet_picked;

/* Set the four globals above from an A.B.C.D-style IP, treating it as a
   /24 (since that is the only prefix we hand out today). Idempotent --
   only the first non-empty seed wins.

   Called from asb_init after load_vm_list with the first saved VM's IP,
   so the existing network's subnet is always adopted across restarts. */
void hcn_seed_nat_subnet_from_ip(const char *vm_nat_ip)
{
    int a, b, c, d;
    if (g_nat_subnet_picked) return;
    if (!vm_nat_ip || vm_nat_ip[0] == '\0') return;
    if (sscanf_s(vm_nat_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return;
    sprintf_s(g_nat_subnet_prefix, sizeof(g_nat_subnet_prefix), "%d.%d.%d.0/24", a, b, c);
    sprintf_s(g_nat_gateway,       sizeof(g_nat_gateway),       "%d.%d.%d.1",    a, b, c);
    sprintf_s(g_nat_ip_base,       sizeof(g_nat_ip_base),       "%d.%d.%d.",     a, b, c);
    g_nat_prefix_len    = 24;
    g_nat_subnet_picked = TRUE;
    ui_log(L"NAT subnet adopted from saved VM IP %S: %S",
            vm_nat_ip, g_nat_subnet_prefix);
}

typedef struct {
    DWORD network;     /* base, host byte order */
    int   prefix_len;
} SubnetInfo;

static BOOL ranges_overlap(DWORD a_net, int a_prefix, DWORD b_net, int b_prefix)
{
    DWORD a_mask = (a_prefix == 0) ? 0 : (~0u << (32 - a_prefix));
    DWORD b_mask = (b_prefix == 0) ? 0 : (~0u << (32 - b_prefix));
    DWORD a_start = a_net & a_mask;
    DWORD a_end   = a_start | ~a_mask;
    DWORD b_start = b_net & b_mask;
    DWORD b_end   = b_start | ~b_mask;
    return (a_start <= b_end) && (b_start <= a_end);
}

/* Collect every IPv4 unicast address bound to a host adapter, plus its
 * on-link prefix. Returns count (clamped to cap). */
static int collect_inuse_subnets(SubnetInfo *out, int cap)
{
    ULONG size = 0;
    int n = 0;
    PIP_ADAPTER_ADDRESSES buf = NULL;
    PIP_ADAPTER_ADDRESSES a;

    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                          NULL, NULL, &size);
    if (size == 0) return 0;
    buf = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if (!buf) return 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                              NULL, buf, &size) != NO_ERROR) {
        free(buf);
        return 0;
    }

    for (a = buf; a && n < cap; a = a->Next) {
        PIP_ADAPTER_UNICAST_ADDRESS u;
        for (u = a->FirstUnicastAddress; u && n < cap; u = u->Next) {
            SOCKADDR_IN *sin;
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sin = (SOCKADDR_IN *)u->Address.lpSockaddr;
            out[n].network    = ntohl(sin->sin_addr.s_addr);
            out[n].prefix_len = u->OnLinkPrefixLength;
            n++;
        }
    }
    free(buf);
    return n;
}

static void ensure_nat_subnet_picked(void)
{
    SubnetInfo used[128];
    int n_used;
    int n;

    if (g_nat_subnet_picked) return;

    n_used = collect_inuse_subnets(used, 128);

    for (n = 42; n <= 255; n++) {
        DWORD candidate = (192u << 24) | (168u << 16) | ((DWORD)n << 8);
        BOOL conflict = FALSE;
        int i;
        for (i = 0; i < n_used; i++) {
            if (ranges_overlap(candidate, 24, used[i].network, used[i].prefix_len)) {
                conflict = TRUE;
                break;
            }
        }
        if (!conflict) {
            sprintf_s(g_nat_subnet_prefix, sizeof(g_nat_subnet_prefix), "192.168.%d.0/24", n);
            sprintf_s(g_nat_gateway,       sizeof(g_nat_gateway),       "192.168.%d.1",    n);
            sprintf_s(g_nat_ip_base,       sizeof(g_nat_ip_base),       "192.168.%d.",     n);
            g_nat_prefix_len    = 24;
            g_nat_subnet_picked = TRUE;
            ui_log(L"NAT subnet selected: %S (gateway %S)",
                    g_nat_subnet_prefix, g_nat_gateway);
            return;
        }
    }

    /* All of 192.168.42-255.0/24 conflict somehow. Fall back to the default
       and let HCN complain in the rare edge case where every /24 in the
       upper half of 192.168/16 is taken. */
    sprintf_s(g_nat_subnet_prefix, sizeof(g_nat_subnet_prefix), "192.168.42.0/24");
    sprintf_s(g_nat_gateway,       sizeof(g_nat_gateway),       "192.168.42.1");
    sprintf_s(g_nat_ip_base,       sizeof(g_nat_ip_base),       "192.168.42.");
    g_nat_prefix_len    = 24;
    g_nat_subnet_picked = TRUE;
    ui_log(L"NAT subnet: no clean /24 in 192.168.42-255, falling back to %S",
            g_nat_subnet_prefix);
}

const char *hcn_nat_subnet_prefix(void) { ensure_nat_subnet_picked(); return g_nat_subnet_prefix; }
const char *hcn_nat_gateway(void)       { ensure_nat_subnet_picked(); return g_nat_gateway; }
const char *hcn_nat_ip_base(void)       { ensure_nat_subnet_picked(); return g_nat_ip_base; }
int         hcn_nat_prefix_len(void)    { ensure_nat_subnet_picked(); return g_nat_prefix_len; }

HRESULT hcn_create_nat_network(GUID *network_id)
{
    wchar_t settings[1024];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_NAT_GUID;

    /* Reuse the existing AppSandbox NAT network if it's already up -
       multiple VMs share one network, each with its own endpoint. */
    if (hcn_network_exists(&APPSANDBOX_NAT_GUID))
        return S_OK;

    ensure_nat_subnet_picked();

    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxNAT\","
        L"\"Type\":\"NAT\","
        L"\"Ipams\":[{"
            L"\"Type\":\"Static\","
            L"\"Subnets\":[{"
                L"\"IpAddressPrefix\":\"%S\","
                L"\"Routes\":[{\"NextHop\":\"%S\",\"DestinationPrefix\":\"0.0.0.0/0\"}]"
            L"}]"
        L"}]"
        L"}",
        g_nat_subnet_prefix, g_nat_gateway);

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN NAT error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet)
        pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_internal_network(GUID *network_id)
{
    wchar_t settings[1024];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_INTERNAL_GUID;

    if (hcn_network_exists(&APPSANDBOX_INTERNAL_GUID))
        return S_OK;

    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxInternal\","
        L"\"Type\":\"ICS\""
        L"}");

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN Internal error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_external_network(GUID *network_id, const wchar_t *adapter_name)
{
    wchar_t settings[2048];
    wchar_t adapter[256];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_EXTERNAL_GUID;

    if (hcn_network_exists(&APPSANDBOX_EXTERNAL_GUID))
        return S_OK;

    /* Use specified adapter, or auto-detect */
    if (adapter_name && adapter_name[0] != L'\0') {
        wcscpy_s(adapter, 256, adapter_name);
        ui_log(L"Using adapter: %s", adapter);
    } else {
        if (!get_default_adapter_name(adapter, 256)) {
            ui_log(L"Error: No connected network adapter found for External network.");
            return E_FAIL;
        }
    }

    swprintf_s(settings, 2048,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxExternal\","
        L"\"Type\":\"Transparent\","
        L"\"Policies\":[{\"Type\":\"NetAdapterName\",\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]"
        L"}", adapter);

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN External error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_endpoint(const GUID *network_id, GUID *endpoint_id,
                            wchar_t *endpoint_guid_str, size_t str_len,
                            const char *nat_ip)
{
    wchar_t net_guid_str[64];
    wchar_t ep_guid_str[64];
    wchar_t settings[1024];
    void *network = NULL;
    void *endpoint = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateEp || !pfnOpenNet)
        return E_NOT_VALID_STATE;

    /* Open the network */
    hr = pfnOpenNet(network_id, &network, &error_record);
    if (error_record) { LocalFree(error_record); error_record = NULL; }
    if (FAILED(hr)) return hr;

    CoCreateGuid(endpoint_id);
    guid_to_string(network_id, net_guid_str, 64);
    guid_to_string(endpoint_id, ep_guid_str, 64);

    /* Static IP for NAT; DHCP for Internal (ICS) and External (Transparent).
       Prefix length matches the picked subnet (see hcn_nat_prefix_len). */
    if (IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID) && nat_ip && nat_ip[0]) {
        ensure_nat_subnet_picked();
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\","
            L"\"IpConfigurations\":[{\"IpAddress\":\"%S\",\"PrefixLength\":%d}]"
            L"}", net_guid_str, nat_ip, g_nat_prefix_len);
    } else if (IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID)) {
        char default_ip[32];
        ensure_nat_subnet_picked();
        sprintf_s(default_ip, sizeof(default_ip), "%s2", g_nat_ip_base);
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\","
            L"\"IpConfigurations\":[{\"IpAddress\":\"%S\",\"PrefixLength\":%d}]"
            L"}", net_guid_str, default_ip, g_nat_prefix_len);
    } else {
        /* Internal (ICS DHCP) or External (LAN DHCP) - no static IP */
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\""
            L"}", net_guid_str);
    }

    hr = pfnCreateEp(network, endpoint_id, settings, &endpoint, &error_record);

    if (SUCCEEDED(hr) && endpoint_guid_str) {
        wcscpy_s(endpoint_guid_str, str_len, ep_guid_str);
    }

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN Endpoint error: %s", error_record);
        LocalFree(error_record);
    }
    if (endpoint && pfnCloseEp) pfnCloseEp(endpoint);
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_delete_network(const GUID *network_id)
{
    PWSTR error_record = NULL;
    HRESULT hr;
    wchar_t guid_str[64];

    if (!g_hcn_dll || !pfnDeleteNet)
        return E_NOT_VALID_STATE;

    StringFromGUID2(network_id, guid_str, 64);
    ui_log(L"HCN: Deleting network %s...", guid_str);

    hr = pfnDeleteNet(network_id, &error_record);
    if (SUCCEEDED(hr)) {
        ui_log(L"HCN: Network deleted.");
    } else {
        ui_log(L"HCN: Delete network failed (0x%08X).", hr);
        if (error_record) {
            ui_log(L"HCN error: %s", error_record);
        }
    }
    if (error_record) LocalFree(error_record);
    return hr;
}

HRESULT hcn_delete_endpoint(const GUID *endpoint_id)
{
    PWSTR error_record = NULL;
    HRESULT hr;
    wchar_t guid_str[64];

    if (!g_hcn_dll || !pfnDeleteEp)
        return E_NOT_VALID_STATE;

    StringFromGUID2(endpoint_id, guid_str, 64);
    ui_log(L"HCN: Deleting endpoint %s...", guid_str);

    hr = pfnDeleteEp(endpoint_id, &error_record);
    if (SUCCEEDED(hr)) {
        ui_log(L"HCN: Endpoint deleted.");
    } else {
        ui_log(L"HCN: Delete endpoint failed (0x%08X).", hr);
        if (error_record) {
            ui_log(L"HCN error: %s", error_record);
        }
    }
    if (error_record) LocalFree(error_record);
    return hr;
}

int hcn_enum_adapters(HcnAdapterCallback cb, void *ctx)
{
    ULONG buf_len = 15000;
    PIP_ADAPTER_ADDRESSES addrs, cur;
    DWORD ret;
    int count = 0;

    addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
    if (!addrs) return 0;

    ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs) return 0;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, addrs, &buf_len);
    }

    if (ret == ERROR_SUCCESS) {
        for (cur = addrs; cur != NULL; cur = cur->Next) {
            if (cur->OperStatus != IfOperStatusUp) continue;
            if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (cur->IfType == IF_TYPE_TUNNEL) continue;
            if (wcsstr(cur->FriendlyName, L"vEthernet") != NULL) continue;

            cb(cur->FriendlyName, (int)cur->IfType, ctx);
            count++;
        }
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return count;
}
