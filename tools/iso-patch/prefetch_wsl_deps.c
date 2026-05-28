/* prefetch_wsl_deps.c - see header.
 *
 * Replicates the manual PowerShell recipe at tools/wsl-deps/README.md
 * in pure C. Versions + flat2 NuGet endpoint paths come straight from
 * that README. .nupkg files are zip archives — extracted with the
 * Windows-builtin tar.exe (libarchive handles zip natively).
 */

#include "prefetch_wsl_deps.h"
#include "iso_patch_log.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

/* Public Azure DevOps NuGet flat-container endpoint. Anonymous, no
 * auth — same URLs the open-source microsoft/WSL repo uses when
 * building its MSI. */
#define WSL_FEED \
    L"https://pkgs.dev.azure.com/shine-oss/13eb32df-d33f-470f-b930-499535a958b4" \
    L"/_packaging/7925a3a1-b93c-4977-8a97-5b877bf2068b/nuget/v3/flat2"

/* Pinned versions — bump in sync with what microsoft/WSL ships.
 * Currently the only published versions on this feed. */
#define D3D_VER     L"1.611.1-81528511"
#define DXCORE_VER  L"10.0.26100.1-240331-1435.ge-release"

/* ====================================================================
 * Shared helpers (small subset of what prefetch_build_deps.c has —
 * not refactored into a common header because that's two helpers).
 * ==================================================================== */

static int u_run_cmd(const wchar_t *cmdline)
{
    wchar_t mut[2048];
    wcsncpy_s(mut, 2048, cmdline, _TRUNCATE);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_err(L"wsl-deps: CreateProcess failed: %lu (%s)", GetLastError(), cmdline);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

static BOOL u_mkdir_p(const wchar_t *path)
{
    wchar_t buf[MAX_PATH];
    wcsncpy_s(buf, MAX_PATH, path, _TRUNCATE);
    wchar_t *p = buf;
    if (wcslen(buf) >= 3 && buf[1] == L':' && buf[2] == L'\\') p = buf + 3;
    for (; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(buf, NULL); *p = L'\\'; }
    }
    CreateDirectoryW(buf, NULL);
    return TRUE;
}

static int parse_url(const wchar_t *url,
                     wchar_t *host_out, size_t host_cap,
                     INTERNET_PORT *port_out,
                     wchar_t *path_out, size_t path_cap,
                     BOOL *secure_out)
{
    const wchar_t *p = url;
    if (_wcsnicmp(p, L"https://", 8) == 0) { p += 8; *port_out = 443; *secure_out = TRUE; }
    else if (_wcsnicmp(p, L"http://", 7) == 0) { p += 7; *port_out = 80; *secure_out = FALSE; }
    else { return -1; }
    const wchar_t *slash = wcschr(p, L'/');
    size_t host_len = slash ? (size_t)(slash - p) : wcslen(p);
    if (host_len >= host_cap) return -1;
    memcpy(host_out, p, host_len * sizeof(wchar_t));
    host_out[host_len] = 0;
    wchar_t *colon = wcschr(host_out, L':');
    if (colon) { *colon = 0; *port_out = (INTERNET_PORT)_wtoi(colon + 1); }
    if (slash) wcsncpy_s(path_out, path_cap, slash, _TRUNCATE);
    else       wcscpy_s(path_out, path_cap, L"/");
    return 0;
}

static int http_download(const wchar_t *url, const wchar_t *out_path)
{
    wchar_t host[256], path[2048];
    INTERNET_PORT port = 443;
    BOOL secure = TRUE;
    if (parse_url(url, host, ARRAYSIZE(host), &port, path, ARRAYSIZE(path), &secure) != 0) {
        log_err(L"wsl-deps: bad URL: %s", url);
        return -1;
    }

    HINTERNET hSession = WinHttpOpen(L"AppSandbox-iso-patch/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { log_err(L"wsl-deps: WinHttpOpen failed: %lu", GetLastError()); return -1; }

    int rc = -1;
    HINTERNET hConn = WinHttpConnect(hSession, host, port, 0);
    if (!hConn) { log_err(L"wsl-deps: WinHttpConnect %s:%u failed: %lu",
                          host, port, GetLastError()); goto cleanup_sess; }

    DWORD reqFlags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        reqFlags);
    if (!hReq) { log_err(L"wsl-deps: WinHttpOpenRequest failed: %lu", GetLastError()); goto cleanup_conn; }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log_err(L"wsl-deps: SendRequest %s failed: %lu", url, GetLastError());
        goto cleanup_req;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        log_err(L"wsl-deps: ReceiveResponse failed: %lu", GetLastError());
        goto cleanup_req;
    }

    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        log_err(L"wsl-deps: HTTP %lu for %s", status, url);
        goto cleanup_req;
    }

    HANDLE hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_err(L"wsl-deps: CreateFileW(%s) failed: %lu", out_path, GetLastError());
        goto cleanup_req;
    }
    BYTE buf[16384];
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { CloseHandle(hFile); goto cleanup_req; }
        if (avail == 0) break;
        DWORD n = avail > sizeof(buf) ? sizeof(buf) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, n, &read)) { CloseHandle(hFile); goto cleanup_req; }
        if (read == 0) break;
        DWORD wrote = 0;
        if (!WriteFile(hFile, buf, read, &wrote, NULL) || wrote != read) {
            CloseHandle(hFile); goto cleanup_req;
        }
    }
    CloseHandle(hFile);
    rc = 0;

cleanup_req:  WinHttpCloseHandle(hReq);
cleanup_conn: WinHttpCloseHandle(hConn);
cleanup_sess: WinHttpCloseHandle(hSession);
    return rc;
}

/* ====================================================================
 * Main flow
 * ==================================================================== */

int do_prefetch_wsl_deps(const wchar_t *out_dir)
{
    log_msg(L"wsl-deps: target %s", out_dir);

    /* No cache layer: always download fresh into out_dir for this VM. */
    u_mkdir_p(out_dir);

    /* Temp dir for nupkg download + extract; wiped on completion. */
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return -1;
    swprintf_s(tmp + wcslen(tmp), MAX_PATH - wcslen(tmp),
               L"isopatch-wsl-deps-%lu", GetTickCount());
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
        CreateDirectoryW(tmp, NULL);
    }

    /* ---- 1. Build URLs + dest paths ---- */
    wchar_t d3d_url[1024], dxcore_url[1024];
    swprintf_s(d3d_url, 1024,
        WSL_FEED L"/microsoft.direct3d.linux/" D3D_VER
        L"/microsoft.direct3d.linux." D3D_VER L".nupkg");
    swprintf_s(dxcore_url, 1024,
        WSL_FEED L"/microsoft.dxcore.linux.amd64fre/" DXCORE_VER
        L"/microsoft.dxcore.linux.amd64fre." DXCORE_VER L".nupkg");

    wchar_t d3d_pkg[MAX_PATH], dxcore_pkg[MAX_PATH];
    wchar_t d3d_extract[MAX_PATH], dxcore_extract[MAX_PATH];
    swprintf_s(d3d_pkg, MAX_PATH, L"%s\\direct3d.nupkg", tmp);
    swprintf_s(dxcore_pkg, MAX_PATH, L"%s\\dxcore.nupkg", tmp);
    swprintf_s(d3d_extract, MAX_PATH, L"%s\\direct3d", tmp);
    swprintf_s(dxcore_extract, MAX_PATH, L"%s\\dxcore", tmp);

    /* ---- 2. Download both .nupkg files ---- */
    log_msg(L"wsl-deps: downloading microsoft.direct3d.linux." D3D_VER L".nupkg");
    if (http_download(d3d_url, d3d_pkg) != 0) {
        log_err(L"wsl-deps: direct3d download failed");
        return -1;
    }
    log_msg(L"wsl-deps: downloading microsoft.dxcore.linux.amd64fre." DXCORE_VER L".nupkg");
    if (http_download(dxcore_url, dxcore_pkg) != 0) {
        log_err(L"wsl-deps: dxcore download failed");
        return -1;
    }

    /* ---- 3. Extract via tar.exe (handles zip via libarchive) ---- */
    {
        wchar_t sys_dir[MAX_PATH], cmd[2048];
        GetSystemDirectoryW(sys_dir, MAX_PATH);
        CreateDirectoryW(d3d_extract, NULL);
        CreateDirectoryW(dxcore_extract, NULL);

        swprintf_s(cmd, 2048,
            L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"",
            sys_dir, d3d_pkg, d3d_extract);
        if (u_run_cmd(cmd) != 0) {
            log_err(L"wsl-deps: tar -xf direct3d.nupkg failed");
            return -1;
        }
        swprintf_s(cmd, 2048,
            L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"",
            sys_dir, dxcore_pkg, dxcore_extract);
        if (u_run_cmd(cmd) != 0) {
            log_err(L"wsl-deps: tar -xf dxcore.nupkg failed");
            return -1;
        }
    }

    /* ---- 4. Copy the 3 .so files directly into out_dir (no cache layout).
     *
     * direct3d nupkg has build/native/lib/x64/{libd3d12.so, libd3d12core.so}.
     * dxcore   nupkg has build/native/lib/libDXCore.so (rename to lowercase).
     */
    struct copy_spec {
        const wchar_t *extract_root;
        const wchar_t *src_rel;
        const wchar_t *dst_name;
    };
    const struct copy_spec specs[] = {
        { d3d_extract,    L"build\\native\\lib\\x64\\libd3d12.so",     L"libd3d12.so"     },
        { d3d_extract,    L"build\\native\\lib\\x64\\libd3d12core.so", L"libd3d12core.so" },
        { dxcore_extract, L"build\\native\\lib\\libDXCore.so",         L"libdxcore.so"    }, /* lowercase */
    };
    int copied = 0;
    for (int i = 0; i < (int)(sizeof(specs) / sizeof(specs[0])); i++) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        swprintf_s(src, MAX_PATH, L"%s\\%s", specs[i].extract_root, specs[i].src_rel);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", out_dir, specs[i].dst_name);
        if (!CopyFileW(src, dst, FALSE)) {
            log_err(L"wsl-deps: CopyFileW %s -> %s failed: %lu",
                    src, dst, GetLastError());
            return -1;
        }
        copied++;
    }

    /* Clean up temp (nupkg files + extracted trees). */
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
    }

    log_msg(L"wsl-deps: OK — %d .so file(s) at %s", copied, out_dir);
    return 0;
}
