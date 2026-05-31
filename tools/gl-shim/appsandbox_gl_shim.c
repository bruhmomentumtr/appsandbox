/*
 * appsandbox_gl_shim.dll
 *
 * Loaded into every user32-linking process via AppInit_DLLs. IAT-hooks
 * opengl32.dll's import of gdi32!D3DKMTQueryAdapterInfo. When opengl32
 * queries KMTQAITYPE_UMOPENGLINFO to learn which ICD to load, the hook
 * rewrites the returned UmdOpenGlIcdFileName to point at OpenGLOn12.dll
 * (the Mesa GL-on-D3D12 ICD that AppSandbox stages into the guest).
 *
 * Inside a GPU-PV guest the kernel advertises the host's native OpenGL ICD
 * (e.g. nvoglv64.dll, projected via HostDriverStore) which cannot initialise
 * because the guest has no matching kernel driver, so opengl32 falls back to
 * GDI Generic 1.1 software. Redirecting the ICD to OpenGLOn12.dll routes GL
 * through D3D12 -> GPU-PV -> host GPU instead.
 *
 * Defensive throughout -- this loads into *every* user32-using process
 * (including system services) and must never throw or crash any process.
 *
 * Build: MSVC, x64, /MT (static CRT so there is no runtime-DLL dependency in
 * arbitrary guest processes). Exposes no exports; all work happens in DllMain.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* ---- D3DKMT ABI ---- */
typedef LONG  NTSTATUS;
typedef ULONG D3DKMT_HANDLE;
#define KMTQAITYPE_UMOPENGLINFO 2

typedef struct {
    D3DKMT_HANDLE hAdapter;
    INT           Type;
    PVOID         pPrivateDriverData;
    UINT          PrivateDriverDataSize;
} D3DKMT_QUERYADAPTERINFO_T;

typedef struct {
    WCHAR UmdOpenGlIcdFileName[260];
    ULONG Version;
    ULONG Flags;
} D3DKMT_OPENGLINFO_T;

typedef NTSTATUS (APIENTRY *PFN_D3DKMTQueryAdapterInfo)(D3DKMT_QUERYADAPTERINFO_T*);

/* ---- ntdll LdrRegisterDllNotification ABI ---- */
typedef struct _UNICODE_STRING_T {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING_T, *PUNICODE_STRING_T;
typedef const UNICODE_STRING_T* PCUNICODE_STRING_T;

typedef struct {
    ULONG               Flags;
    PCUNICODE_STRING_T  FullDllName;
    PCUNICODE_STRING_T  BaseDllName;
    PVOID               DllBase;
    ULONG               SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA_T;

#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

typedef VOID (NTAPI *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG, LDR_DLL_NOTIFICATION_DATA_T*, PVOID);
typedef NTSTATUS (NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);

/* ---- Globals ---- */
static PFN_D3DKMTQueryAdapterInfo g_real_query = NULL;
/* TARGET PATH: the OpenGLOn12.dll copy AppSandbox stages into the guest
 * (see disk_util.c manifest: \Windows\AppSandbox\d3dlayers\OpenGLOn12.dll). */
static const WCHAR* g_target_icd =
    L"C:\\Windows\\AppSandbox\\d3dlayers\\OpenGLOn12.dll";

static CRITICAL_SECTION g_log_cs;
static volatile LONG    g_log_init = 0;
static FILE*            g_log_fp   = NULL;
static volatile LONG    g_hook_installed = 0;
static PVOID            g_ldr_cookie = NULL;

/* ---- Logging (one log file per PID under C:\ProgramData\appsandbox_shim) ---- */
static void ensure_log(void) {
    char dir[MAX_PATH] = "C:\\ProgramData\\appsandbox_shim";
    char exepath[MAX_PATH] = {0};
    const char* leaf;
    char path[MAX_PATH];

    if (InterlockedCompareExchange(&g_log_init, 1, 0) != 0) return;
    InitializeCriticalSection(&g_log_cs);

    CreateDirectoryA(dir, NULL);

    GetModuleFileNameA(NULL, exepath, sizeof(exepath));
    leaf = strrchr(exepath, '\\');
    leaf = leaf ? leaf + 1 : exepath;
    snprintf(path, sizeof(path), "%s\\%lu_%s.log", dir,
             (unsigned long)GetCurrentProcessId(), leaf);
    g_log_fp = fopen(path, "a");
}

static void shimlog(const char* fmt, ...) {
    va_list ap;
    char ts[64];
    SYSTEMTIME t;

    ensure_log();
    if (!g_log_fp) return;
    EnterCriticalSection(&g_log_cs);
    GetLocalTime(&t);
    snprintf(ts, sizeof(ts), "%02u:%02u:%02u.%03u",
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    fprintf(g_log_fp, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_log_fp);
    fflush(g_log_fp);
    LeaveCriticalSection(&g_log_cs);
}

/* ---- The hook ---- */
static NTSTATUS APIENTRY HookedQueryAdapterInfo(D3DKMT_QUERYADAPTERINFO_T* q) {
    NTSTATUS r;
    if (!g_real_query) return (NTSTATUS)0xC0000001L;
    r = g_real_query(q);
    if (r == 0 && q && q->Type == KMTQAITYPE_UMOPENGLINFO &&
        q->pPrivateDriverData &&
        q->PrivateDriverDataSize >= sizeof(D3DKMT_OPENGLINFO_T))
    {
        D3DKMT_OPENGLINFO_T* gl = (D3DKMT_OPENGLINFO_T*)q->pPrivateDriverData;
        WCHAR before[260];
        wcsncpy(before, gl->UmdOpenGlIcdFileName, 260); before[259] = 0;
        wcsncpy(gl->UmdOpenGlIcdFileName, g_target_icd, 260);
        gl->UmdOpenGlIcdFileName[259] = 0;
        shimlog("UMOPENGLINFO rewrite: \"%ls\" -> \"%ls\" (hAdapter=0x%lx)",
                before, g_target_icd, (unsigned long)q->hAdapter);
    }
    return r;
}

/* ---- IAT patch on opengl32's import of gdi32!D3DKMTQueryAdapterInfo ---- */
static int patch_iat(HMODULE target) {
    BYTE* base = (BYTE*)target;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt;
    IMAGE_DATA_DIRECTORY* d;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    int patched = 0;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d->VirtualAddress || !d->Size) return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d->VirtualAddress);
    for (; imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        IMAGE_THUNK_DATA* origT;
        IMAGE_THUNK_DATA* iatT;
        if (_stricmp(dll, "gdi32.dll") != 0) continue;

        origT = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        iatT  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; origT->u1.AddressOfData; origT++, iatT++) {
            IMAGE_IMPORT_BY_NAME* by;
            DWORD oldProt;
            void* prev;
            if (IMAGE_SNAP_BY_ORDINAL(origT->u1.Ordinal)) continue;
            by = (IMAGE_IMPORT_BY_NAME*)(base + origT->u1.AddressOfData);
            if (strcmp((const char*)by->Name, "D3DKMTQueryAdapterInfo") != 0) continue;

            if (!VirtualProtect(&iatT->u1.Function, sizeof(uintptr_t),
                                PAGE_READWRITE, &oldProt)) return 0;
            prev = (void*)(uintptr_t)iatT->u1.Function;
            if (!g_real_query) g_real_query = (PFN_D3DKMTQueryAdapterInfo)prev;
            iatT->u1.Function = (uintptr_t)HookedQueryAdapterInfo;
            VirtualProtect(&iatT->u1.Function, sizeof(uintptr_t),
                           oldProt, &oldProt);
            patched = 1;
            shimlog("IAT patched: opengl32!gdi32.D3DKMTQueryAdapterInfo prev=%p new=%p",
                    prev, (void*)HookedQueryAdapterInfo);
        }
    }
    return patched;
}

static void try_hook_opengl32(void) {
    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    if (!m) return;
    if (InterlockedExchange(&g_hook_installed, 1) != 0) return;
    if (!patch_iat(m)) {
        InterlockedExchange(&g_hook_installed, 0);
        shimlog("opengl32 IAT patch FAILED");
    }
}

/* ---- LdrRegisterDllNotification: catch later opengl32 loads ---- */
static VOID NTAPI dll_notification(ULONG reason,
                                   LDR_DLL_NOTIFICATION_DATA_T* data,
                                   PVOID ctx)
{
    PCUNICODE_STRING_T n;
    (void)ctx;
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED) return;
    if (!data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
    n = data->BaseDllName;
    if (n->Length / sizeof(WCHAR) >= 8 &&
        _wcsnicmp(n->Buffer, L"opengl32", 8) == 0)
    {
        shimlog("ldr notify: opengl32.dll loaded at %p", data->DllBase);
        try_hook_opengl32();
    }
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID resvd) {
    (void)resvd;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE ntdll;
        DisableThreadLibraryCalls(hMod);
        shimlog("PROCESS_ATTACH");
        try_hook_opengl32();
        ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            PFN_LdrRegisterDllNotification reg =
                (PFN_LdrRegisterDllNotification)(uintptr_t)
                GetProcAddress(ntdll, "LdrRegisterDllNotification");
            if (reg) {
                NTSTATUS s = reg(0, dll_notification, NULL, &g_ldr_cookie);
                shimlog("LdrRegisterDllNotification status=0x%08lx cookie=%p",
                        (unsigned long)s, g_ldr_cookie);
            }
        }
    }
    return TRUE;
}
