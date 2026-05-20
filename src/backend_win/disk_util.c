#include "disk_util.h"
#include "ui.h"
#include <virtdisk.h>
#include <stdio.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <imapi2fs.h>

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "urlmon.lib")

#include <urlmon.h>

#define SSH_MSI_URL     L"https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64-v10.0.0.0.msi"
#define SSH_MSI_NAME    L"OpenSSH-Win64-v10.0.0.0.msi"

static const GUID VHDX_VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

HRESULT vhdx_create(const wchar_t *path, ULONGLONG size_gb)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!path || size_gb == 0)
        return E_INVALIDARG;

    /* Reuse existing VHDX if it already exists */
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return S_OK;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.MaximumSize = size_gb * 1024ULL * 1024ULL * 1024ULL;
    params.Version2.BlockSizeInBytes = 0; /* default */
    params.Version2.SectorSizeInBytes = 512;
    params.Version2.PhysicalSectorSizeInBytes = 4096;

    result = CreateVirtualDisk(
        &storage_type,
        path,
        VIRTUAL_DISK_ACCESS_NONE,
        NULL,
        CREATE_VIRTUAL_DISK_FLAG_NONE, /* dynamically expanding */
        0,
        &params,
        NULL,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    CloseHandle(vhd_handle);
    return S_OK;
}

HRESULT vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!child_path || !parent_path)
        return E_INVALIDARG;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.ParentPath = parent_path;

    result = CreateVirtualDisk(
        &storage_type,
        child_path,
        VIRTUAL_DISK_ACCESS_NONE,
        NULL,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &params,
        NULL,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    CloseHandle(vhd_handle);
    return S_OK;
}

HRESULT vhdx_create_differencing_resized(const wchar_t *child_path,
                                          const wchar_t *parent_path,
                                          ULONGLONG size_gb)
{
    HRESULT hr;
    VIRTUAL_STORAGE_TYPE storage_type;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    RESIZE_VIRTUAL_DISK_PARAMETERS resize_params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;
    ULONGLONG target_bytes;

    /* Step 1: plain differencing-create. Child inherits parent's MaximumSize.
       For Ubuntu cloud-image parent that's ~2.5 GB, which isn't enough for
       an ubuntu-desktop-minimal install. */
    hr = vhdx_create_differencing(child_path, parent_path);
    if (FAILED(hr)) return hr;

    /* Step 2: optional resize. size_gb == 0 means "keep parent's size".
       Skipping the resize is also the right thing if the user-requested
       size is <= what the child already inherits (no shrinking needed). */
    if (size_gb == 0) return S_OK;
    target_bytes = size_gb * 1024ULL * 1024ULL * 1024ULL;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.GetInfoOnly = FALSE;

    result = OpenVirtualDisk(
        &storage_type,
        child_path,
        VIRTUAL_DISK_ACCESS_NONE,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &open_params,
        &vhd_handle);
    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    ZeroMemory(&resize_params, sizeof(resize_params));
    resize_params.Version = RESIZE_VIRTUAL_DISK_VERSION_1;
    resize_params.Version1.NewSize = target_bytes;

    /* Synchronous resize. ResizeVirtualDisk works on differencing VHDX on
       Win8.1+ for growing (parent stays the same; child gets a larger
       MaximumSize). Caller is expected to follow up with cloud-init's
       growpart + resizefs inside the VM to actually extend the rootfs. */
    result = ResizeVirtualDisk(
        vhd_handle,
        RESIZE_VIRTUAL_DISK_FLAG_NONE,
        &resize_params,
        NULL);

    CloseHandle(vhd_handle);

    if (result != ERROR_SUCCESS) {
        /* Resize failed — child remains at parent's size. Delete it so the
           caller doesn't end up with an undersized disk silently. */
        DeleteFileW(child_path);
        return HRESULT_FROM_WIN32(result);
    }
    return S_OK;
}

HRESULT vhdx_merge(const wchar_t *child_path)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    MERGE_VIRTUAL_DISK_PARAMETERS merge_params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!child_path)
        return E_INVALIDARG;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;

    result = OpenVirtualDisk(
        &storage_type,
        child_path,
        VIRTUAL_DISK_ACCESS_NONE,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &open_params,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    ZeroMemory(&merge_params, sizeof(merge_params));
    merge_params.Version = MERGE_VIRTUAL_DISK_VERSION_1;
    merge_params.Version1.MergeDepth = 1;

    result = MergeVirtualDisk(
        vhd_handle,
        MERGE_VIRTUAL_DISK_FLAG_NONE,
        &merge_params,
        NULL);

    CloseHandle(vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    return S_OK;
}

/* ---- Resources ISO for unattended install ---- */

/* Windows unattend base64 password encoding:
   UTF-16LE( password + "Password" ) -> base64.
   When PlainText=false, Windows Setup decodes this at runtime. */
static BOOL encode_unattend_password(const wchar_t *pass, wchar_t *b64_out, int b64_max)
{
    wchar_t combined[256];
    DWORD bin_len, b64_len;
    char *b64_narrow;

    swprintf_s(combined, 256, L"%sPassword", pass);
    bin_len = (DWORD)(wcslen(combined) * sizeof(wchar_t));

    /* Get required base64 length */
    b64_len = 0;
    CryptBinaryToStringA((const BYTE *)combined, bin_len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len);
    b64_narrow = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64_narrow) {
        SecureZeroMemory(combined, sizeof(combined));
        return FALSE;
    }

    CryptBinaryToStringA((const BYTE *)combined, bin_len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64_narrow, &b64_len);
    SecureZeroMemory(combined, sizeof(combined));

    MultiByteToWideChar(CP_UTF8, 0, b64_narrow, -1, b64_out, b64_max);
    SecureZeroMemory(b64_narrow, b64_len);
    HeapFree(GetProcessHeap(), 0, b64_narrow);
    return TRUE;
}

/* Map language tag to LCID:KLID format for InputLocale */
static const wchar_t *lang_to_input_locale(const wchar_t *lang)
{
    static const struct { const wchar_t *tag; const wchar_t *klid; } map[] = {
        { L"en-US", L"0409:00000409" }, { L"en-GB", L"0809:00000809" },
        { L"de-DE", L"0407:00000407" }, { L"fr-FR", L"040c:0000040c" },
        { L"fr-CA", L"0c0c:00001009" }, { L"es-ES", L"0c0a:0000040a" },
        { L"es-MX", L"080a:0000080a" }, { L"it-IT", L"0410:00000410" },
        { L"pt-BR", L"0416:00000416" }, { L"pt-PT", L"0816:00000816" },
        { L"ja-JP", L"0411:00000411" }, { L"ko-KR", L"0412:00000412" },
        { L"zh-CN", L"0804:00000804" }, { L"zh-TW", L"0404:00000404" },
        { L"ru-RU", L"0419:00000419" }, { L"pl-PL", L"0415:00000415" },
        { L"nl-NL", L"0413:00000413" }, { L"sv-SE", L"041d:0000041d" },
        { L"nb-NO", L"0414:00000414" }, { L"da-DK", L"0406:00000406" },
        { L"fi-FI", L"040b:0000040b" }, { L"cs-CZ", L"0405:00000405" },
        { L"hu-HU", L"040e:0000040e" }, { L"tr-TR", L"041f:0000041f" },
        { L"ar-SA", L"0401:00000401" }, { L"he-IL", L"040d:0000040d" },
        { L"th-TH", L"041e:0000041e" }, { L"uk-UA", L"0422:00000422" },
        { L"ro-RO", L"0418:00000418" }, { L"el-GR", L"0408:00000408" },
        { L"bg-BG", L"0402:00020402" }, { L"hr-HR", L"041a:0000041a" },
        { L"sk-SK", L"041b:0000041b" }, { L"sl-SI", L"0424:00000424" },
        { L"et-EE", L"0425:00000425" }, { L"lv-LV", L"0426:00000426" },
        { L"lt-LT", L"0427:00000427" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (_wcsicmp(lang, map[i].tag) == 0)
            return map[i].klid;
    }
    return L"0409:00000409";  /* fallback */
}

/* Generate autounattend.xml with credentials and VM name substituted.
   If is_template, adds SecureStartup-FilterDriver to disable BitLocker
   (required for sysprep). */
static BOOL generate_autounattend(const wchar_t *output_path,
                                   const wchar_t *vm_name,
                                   const wchar_t *admin_user,
                                   const wchar_t *b64_password,
                                   BOOL is_template,
                                   BOOL test_mode,
                                   const wchar_t *lang)
{
    FILE *f;
    wchar_t comp_name[16];
    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    /* windowsPE + specialize (up through Deployment component) */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"windowsPE\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core-WinPE\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <SetupUILanguage><UILanguage>%s</UILanguage></SetupUILanguage>\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <UserData>\n"
        L"                <AcceptEula>true</AcceptEula>\n"
        L"                <ProductKey><WillShowUI>Never</WillShowUI></ProductKey>\n"
        L"            </UserData>\n"
        L"            <ImageInstall><OSImage>\n"
        L"                <InstallFrom><MetaData wcm:action=\"add\">\n"
        L"                    <Key>/IMAGE/NAME</Key>\n"
        L"                    <Value>Windows 11 Pro</Value>\n"
        L"                </MetaData></InstallFrom>\n"
        L"                <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>\n"
        L"            </OSImage></ImageInstall>\n"
        L"            <DiskConfiguration><Disk wcm:action=\"add\">\n"
        L"                <DiskID>0</DiskID><WillWipeDisk>true</WillWipeDisk>\n"
        L"                <CreatePartitions>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>\n"
        L"                </CreatePartitions>\n"
        L"                <ModifyPartitions>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>2</Order><PartitionID>2</PartitionID></ModifyPartition>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>3</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>\n"
        L"                </ModifyPartitions>\n"
        L"            </Disk></DiskConfiguration>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        lang, lang_to_input_locale(lang), lang, lang, lang,
        comp_name);

    {
        int order = 2;
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            L"                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fwprintf(f,
                L"                <RunSynchronousCommand wcm:action=\"add\">\n"
                L"                    <Order>%d</Order>\n"
                L"                    <Path>bcdedit /set testsigning on</Path>\n"
                L"                </RunSynchronousCommand>\n", order++);
        }
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>cmd /c mkdir C:\\Windows\\Setup\\Scripts</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>cmd /c for %%d in (D E F G H I J) do @if exist %%d:\\SetupComplete.cmd copy /Y %%d:\\SetupComplete.cmd C:\\Windows\\Setup\\Scripts\\</Path>\n"
            L"                </RunSynchronousCommand>\n",
            order, order + 1);
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n");

    /* Template: disable BitLocker so sysprep can run */
    if (is_template) {
        fwprintf(f,
            L"        <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <PreventDeviceEncryption>true</PreventDeviceEncryption>\n"
            L"        </component>\n");
    }

    fwprintf(f,
        L"    </settings>\n"
        L"\n");

    if (is_template) {
        /* Template: boot into audit mode, run sysprep from auditUser pass.
           No user account or OOBE needed. */
        fwprintf(f,
            L"    <settings pass=\"oobeSystem\">\n"
            L"        <component name=\"Microsoft-Windows-Deployment\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <Reseal>\n"
            L"                <Mode>Audit</Mode>\n"
            L"            </Reseal>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"\n"
            L"    <settings pass=\"auditUser\">\n"
            L"        <component name=\"Microsoft-Windows-Deployment\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <RunSynchronous>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>1</Order>\n"
            L"                    <Description>Generalize VM and shut down for templating</Description>\n"
            L"                    <Path>C:\\Windows\\System32\\Sysprep\\sysprep.exe /generalize /oobe /shutdown /mode:vm</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"            </RunSynchronous>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"</unattend>\n");
    } else {
        /* Normal VM: full OOBE with user account, AutoLogon, agent install */
        fwprintf(f,
            L"    <settings pass=\"oobeSystem\">\n"
            L"        <component name=\"Microsoft-Windows-International-Core\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <InputLocale>%s</InputLocale>\n"
            L"            <SystemLocale>%s</SystemLocale>\n"
            L"            <UILanguage>%s</UILanguage>\n"
            L"            <UserLocale>%s</UserLocale>\n"
            L"        </component>\n"
            L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <OOBE>\n"
            L"                <HideEULAPage>true</HideEULAPage>\n"
            L"                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
            L"                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
            L"                <ProtectYourPC>3</ProtectYourPC>\n"
            L"            </OOBE>\n"
            L"            <UserAccounts><LocalAccounts>\n"
            L"                <LocalAccount wcm:action=\"add\">\n"
            L"                    <Name>%s</Name>\n"
            L"                    <Group>Administrators</Group>\n"
            L"                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
            L"                </LocalAccount>\n"
            L"            </LocalAccounts></UserAccounts>\n"
            L"            <AutoLogon>\n"
            L"                <Enabled>true</Enabled>\n"
            L"                <Username>%s</Username>\n"
            L"                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
            L"                <LogonCount>1</LogonCount>\n"
            L"            </AutoLogon>\n"
            L"            <FirstLogonCommands>\n"
            L"                <SynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>1</Order>\n"
            L"                    <Description>Run AppSandbox setup</Description>\n"
            L"                    <CommandLine>cmd /c \"for %%d in (D E F G H I J) do @if exist %%d:\\setup.cmd call %%d:\\setup.cmd\"</CommandLine>\n"
            L"                    <RequiresUserInput>false</RequiresUserInput>\n"
            L"                </SynchronousCommand>\n"
            L"            </FirstLogonCommands>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"</unattend>\n",
            lang_to_input_locale(lang), lang, lang, lang,
            admin_user, b64_password,
            admin_user, b64_password);
    }

    fclose(f);
    return TRUE;
}

/* ---- ISO creation via IMAPI2 ---- */

/* GUIDs for IMAPI2 file system image COM objects */
static const GUID CLSID_MsftFSImage =
    {0x2C941FC5, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID IID_IFSImage =
    {0x2C941FE1, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};

/* Write an IStream to a file */
static HRESULT write_stream_to_file(IStream *stream, const wchar_t *path)
{
    HANDLE hFile;
    BYTE buf[65536];
    ULONG bytes_read;
    DWORD bytes_written;
    HRESULT hr;
    LARGE_INTEGER zero;

    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    zero.QuadPart = 0;
    stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);

    for (;;) {
        hr = stream->lpVtbl->Read(stream, buf, sizeof(buf), &bytes_read);
        if (FAILED(hr) || bytes_read == 0) break;
        WriteFile(hFile, buf, bytes_read, &bytes_written, NULL);
    }

    CloseHandle(hFile);
    return S_OK;
}

/* Build an ISO9660+Joliet image from a staging directory using IMAPI2 */
static HRESULT create_iso_from_dir(const wchar_t *iso_path, const wchar_t *staging_dir,
                                    const wchar_t *volume_label)
{
    IFileSystemImage *pImage = NULL;
    IFsiDirectoryItem *pRoot = NULL;
    IFileSystemImageResult *pResult = NULL;
    IStream *pStream = NULL;
    BSTR bstrDir = NULL, bstrLabel = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MsftFSImage, NULL, CLSCTX_ALL,
                          &IID_IFSImage, (void **)&pImage);
    if (FAILED(hr)) {
        ui_log(L"Error: CoCreateInstance(MsftFileSystemImage) failed (0x%08X)", hr);
        return hr;
    }

    /* ISO 9660 + Joliet (needed for long filenames like autounattend.xml) */
    pImage->lpVtbl->put_FileSystemsToCreate(pImage,
        FsiFileSystemISO9660 | FsiFileSystemJoliet);

    bstrLabel = SysAllocString(volume_label);
    pImage->lpVtbl->put_VolumeName(pImage, bstrLabel);

    hr = pImage->lpVtbl->get_Root(pImage, &pRoot);
    if (FAILED(hr)) {
        ui_log(L"Error: get_Root failed (0x%08X)", hr);
        goto cleanup;
    }

    bstrDir = SysAllocString(staging_dir);
    hr = pRoot->lpVtbl->AddTree(pRoot, bstrDir, VARIANT_FALSE);
    if (FAILED(hr)) {
        ui_log(L"Error: AddTree failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = pImage->lpVtbl->CreateResultImage(pImage, &pResult);
    if (FAILED(hr)) {
        ui_log(L"Error: CreateResultImage failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = pResult->lpVtbl->get_ImageStream(pResult, &pStream);
    if (FAILED(hr)) {
        ui_log(L"Error: get_ImageStream failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = write_stream_to_file(pStream, iso_path);

cleanup:
    if (pStream) pStream->lpVtbl->Release(pStream);
    if (pResult) pResult->lpVtbl->Release(pResult);
    if (pRoot) pRoot->lpVtbl->Release(pRoot);
    if (pImage) pImage->lpVtbl->Release(pImage);
    SysFreeString(bstrDir);
    SysFreeString(bstrLabel);
    return hr;
}

/* Remove a directory and all files in it (two levels deep for subdirs like drivers\) */
static void remove_staging_dir(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* Recurse one level for subdirectories (e.g. drivers\) */
                remove_staging_dir(full);
            } else {
                DeleteFileW(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir, BOOL ssh_enabled);

/* ---- SSH MSI download / cache ---- */

BOOL ensure_ssh_msi_cached(wchar_t *msi_path_out, int max_chars)
{
    wchar_t exe_dir[MAX_PATH], *slash;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    swprintf_s(msi_path_out, max_chars, L"%s\\%s", exe_dir, SSH_MSI_NAME);

    /* Already cached? */
    if (GetFileAttributesW(msi_path_out) != INVALID_FILE_ATTRIBUTES)
        return TRUE;

    ui_log(L"Downloading OpenSSH MSI...");
    if (URLDownloadToFileW(NULL, SSH_MSI_URL, msi_path_out, 0, NULL) == S_OK) {
        ui_log(L"OpenSSH MSI downloaded.");
        return TRUE;
    }

    ui_log(L"Failed to download OpenSSH MSI.");
    return FALSE;
}

HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode,
                              BOOL ssh_enabled,
                              const wchar_t *lang)
{
    wchar_t dir[MAX_PATH];
    wchar_t staging[MAX_PATH];
    wchar_t b64_pass[512];
    wchar_t file_path[MAX_PATH];
    wchar_t *last_slash;
    HRESULT hr;

    if (!iso_path || !vm_name || !admin_user || !admin_pass)
        return E_INVALIDARG;

    /* Get output directory from iso_path */
    wcscpy_s(dir, MAX_PATH, iso_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));
        return E_FAIL;
    }
    SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));

    /* Delete stale ISO */
    if (GetFileAttributesW(iso_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(iso_path);

    /* Create staging directory */
    swprintf_s(staging, MAX_PATH, L"%s\\_iso_staging", dir);
    CreateDirectoryW(staging, NULL);

    ui_log(L"Creating resources ISO...");

    /* autounattend.xml */
    swprintf_s(file_path, MAX_PATH, L"%s\\autounattend.xml", staging);
    if (!generate_autounattend(file_path, vm_name, admin_user, b64_pass, is_template, test_mode, lang))
        ui_log(L"Warning: failed to write autounattend.xml");

    /* Agent + input helper + VDD driver files + SSH MSI */
    stage_agent_and_setup(staging, res_dir, ssh_enabled);

    /* setup.cmd — first-logon script: installs agent service.
       This runs from the ISO drive, so %~dp0 is the ISO root. */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    {
        FILE *cmd;
        if (_wfopen_s(&cmd, file_path, L"w") == 0 && cmd) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === setup.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install guest agent\r\n"
                "if exist \"%~dp0appsandbox-agent.exe\" (\r\n"
                "    copy /Y \"%~dp0appsandbox-agent.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-input.exe\" copy /Y \"%~dp0appsandbox-input.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-displays.exe\" copy /Y \"%~dp0appsandbox-displays.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard.exe\" copy /Y \"%~dp0appsandbox-clipboard.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard-reader.exe\" copy /Y \"%~dp0appsandbox-clipboard-reader.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-audio.exe\" copy /Y \"%~dp0appsandbox-audio.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    \"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "echo === setup.cmd finished === >> \"%LOG%\"\r\n",
                cmd);
            fclose(cmd);
        } else {
            ui_log(L"Warning: failed to write setup.cmd");
        }
    }

    /* SetupComplete.cmd — runs as SYSTEM after setup, before first logon.
       Copied to C:\Windows\Setup\Scripts\ during specialize pass.
       Installs VDD driver (cert + test signing + devcon) + optionally OpenSSH. */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    {
        FILE *sc;
        if (_wfopen_s(&sc, file_path, L"w") == 0 && sc) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Find ISO drive\r\n"
                "set ISODRV=\r\n"
                "for %%d in (D E F G H I J) do @if exist %%d:\\drivers\\AppSandboxVDD.inf set ISODRV=%%d:\r\n"
                "if \"%ISODRV%\"==\"\" (\r\n"
                "    echo [VDD] Could not find ISO drive with driver files >> \"%LOG%\"\r\n"
                "    goto :done\r\n"
                ")\r\n"
                "echo [VDD] Found driver files on %ISODRV% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Enable test signing\r\n"
                "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
                "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
                "\r\n"
                "REM Install certificate\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" (\r\n"
                "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
                "    certutil -addstore Root \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                "    certutil -f -addstore TrustedPublisher \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "\r\n"
                "REM Install driver with devcon\r\n"
                "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "dir \"%ISODRV%\\drivers\\\" >> \"%LOG%\" 2>&1\r\n"
                "\"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
                "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Disable display sleep so IDD swap chain stays alive\r\n"
                "powercfg /change monitor-timeout-ac 0\r\n"
                "powercfg /change monitor-timeout-dc 0\r\n"
                "powercfg /change standby-timeout-ac 0\r\n"
                "powercfg /change standby-timeout-dc 0\r\n"
                "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install VAD driver with devcon\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" (\r\n"
                "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "    \"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
                "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                ")\r\n"
                "\r\n",
                sc);

            if (ssh_enabled) {
                fputs(
                    "REM Install OpenSSH Server\r\n"
                    "if exist \"%ISODRV%\\OpenSSH-Win64-v10.0.0.0.msi\" (\r\n"
                    "    echo [SSH] Installing OpenSSH Server... >> \"%LOG%\"\r\n"
                    "    msiexec /i \"%ISODRV%\\OpenSSH-Win64-v10.0.0.0.msi\" /qn /norestart >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] msiexec exit code: %errorlevel% >> \"%LOG%\"\r\n"
                    "    sc config sshd start= auto >> \"%LOG%\" 2>&1\r\n"
                    "    net start sshd >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] Done >> \"%LOG%\"\r\n"
                    ")\r\n"
                    "\r\n",
                    sc);
            }

            fputs(
                ":done\r\n"
                "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
                sc);
            fclose(sc);
        }
    }

    /* Build ISO from staging directory */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = create_iso_from_dir(iso_path, staging, L"APPSETUP");
    CoUninitialize();

    /* Clean up staging directory */
    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    remove_staging_dir(staging);

    if (SUCCEEDED(hr))
        ui_log(L"Resources ISO created: %s", iso_path);

    return hr;
}

/* ---- Template VM resources ---- */


/* Generate unattend.xml for instances created from templates.
   Post-sysprep mini-setup uses "unattend.xml" (not "autounattend.xml")
   when searching removable media. Contains specialize + oobeSystem passes. */
static BOOL generate_unattend_instance(const wchar_t *output_path,
                                        const wchar_t *vm_name,
                                        const wchar_t *admin_user,
                                        const wchar_t *b64_password,
                                        const wchar_t *lang)
{
    FILE *f;
    wchar_t comp_name[16];
    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>2</Order>\n"
        L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>3</Order>\n"
        L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>4</Order>\n"
        L"                    <Path>cmd /c mkdir C:\\Windows\\Setup\\Scripts</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>5</Order>\n"
        L"                    <Path>cmd /c for %%d in (D E F G H I J) do @if exist %%d:\\SetupComplete.cmd copy /Y %%d:\\SetupComplete.cmd C:\\Windows\\Setup\\Scripts\\</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <OOBE>\n"
        L"                <HideEULAPage>true</HideEULAPage>\n"
        L"                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
        L"                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
        L"                <ProtectYourPC>3</ProtectYourPC>\n"
        L"            </OOBE>\n"
        L"            <UserAccounts><LocalAccounts>\n"
        L"                <LocalAccount wcm:action=\"add\">\n"
        L"                    <Name>%s</Name>\n"
        L"                    <Group>Administrators</Group>\n"
        L"                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                </LocalAccount>\n"
        L"            </LocalAccounts></UserAccounts>\n"
        L"            <AutoLogon>\n"
        L"                <Enabled>true</Enabled>\n"
        L"                <Username>%s</Username>\n"
        L"                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                <LogonCount>1</LogonCount>\n"
        L"            </AutoLogon>\n"
        L"            <FirstLogonCommands>\n"
        L"                <SynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Description>Run AppSandbox setup</Description>\n"
        L"                    <CommandLine>cmd /c \"for %%d in (D E F G H I J) do @if exist %%d:\\setup.cmd call %%d:\\setup.cmd\"</CommandLine>\n"
        L"                    <RequiresUserInput>false</RequiresUserInput>\n"
        L"                </SynchronousCommand>\n"
        L"            </FirstLogonCommands>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n",
        comp_name,
        lang_to_input_locale(lang), lang, lang, lang,
        admin_user, b64_password,
        admin_user, b64_password);

    fclose(f);
    return TRUE;
}

/* Helper: copy agent exe to staging and write setup.cmd */
static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir, BOOL ssh_enabled)
{
    wchar_t src_path[MAX_PATH], file_path[MAX_PATH];
    BOOL found_agent = FALSE;

    /* appsandbox-agent.exe */
    if (res_dir) {
        swprintf_s(src_path, MAX_PATH, L"%s\\appsandbox-agent.exe", res_dir);
        if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
            found_agent = TRUE;
    }
    if (!found_agent) {
        wchar_t exe_dir[MAX_PATH];
        wchar_t *slash;
        GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
        slash = wcsrchr(exe_dir, L'\\');
        if (slash) *slash = L'\0';
        swprintf_s(src_path, MAX_PATH, L"%s\\appsandbox-agent.exe", exe_dir);
        if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
            found_agent = TRUE;
    }
    if (found_agent) {
        wchar_t input_src[MAX_PATH], input_dst[MAX_PATH];
        swprintf_s(file_path, MAX_PATH, L"%s\\appsandbox-agent.exe", staging);
        if (!CopyFileW(src_path, file_path, FALSE))
            ui_log(L"Warning: failed to copy appsandbox-agent.exe (error %lu)", GetLastError());

        /* appsandbox-input.exe — same directory as agent */
        wcscpy_s(input_src, MAX_PATH, src_path);
        {
            wchar_t *s = wcsrchr(input_src, L'\\');
            if (s) *(s + 1) = L'\0';
        }
        wcscat_s(input_src, MAX_PATH, L"appsandbox-input.exe");
        swprintf_s(input_dst, MAX_PATH, L"%s\\appsandbox-input.exe", staging);
        if (GetFileAttributesW(input_src) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(input_src, input_dst, FALSE))
                ui_log(L"Warning: failed to copy appsandbox-input.exe (error %lu)", GetLastError());
            else
                ui_log(L"Staged appsandbox-input.exe for ISO.");
        } else {
            ui_log(L"Warning: appsandbox-input.exe not found at %s", input_src);
        }

        /* appsandbox-displays.exe — same directory as agent */
        {
            wchar_t displays_src[MAX_PATH], displays_dst[MAX_PATH];
            wcscpy_s(displays_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(displays_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(displays_src, MAX_PATH, L"appsandbox-displays.exe");
            swprintf_s(displays_dst, MAX_PATH, L"%s\\appsandbox-displays.exe", staging);
            if (GetFileAttributesW(displays_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(displays_src, displays_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-displays.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-displays.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-displays.exe not found at %s", displays_src);
            }
        }

        /* appsandbox-clipboard.exe — same directory as agent */
        {
            wchar_t clip_src[MAX_PATH], clip_dst[MAX_PATH];
            wcscpy_s(clip_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(clip_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(clip_src, MAX_PATH, L"appsandbox-clipboard.exe");
            swprintf_s(clip_dst, MAX_PATH, L"%s\\appsandbox-clipboard.exe", staging);
            if (GetFileAttributesW(clip_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(clip_src, clip_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-clipboard.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-clipboard.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-clipboard.exe not found at %s", clip_src);
            }
        }

        /* appsandbox-clipboard-reader.exe — same directory as agent */
        {
            wchar_t reader_src[MAX_PATH], reader_dst[MAX_PATH];
            wcscpy_s(reader_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(reader_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(reader_src, MAX_PATH, L"appsandbox-clipboard-reader.exe");
            swprintf_s(reader_dst, MAX_PATH, L"%s\\appsandbox-clipboard-reader.exe", staging);
            if (GetFileAttributesW(reader_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(reader_src, reader_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-clipboard-reader.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-clipboard-reader.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-clipboard-reader.exe not found at %s", reader_src);
            }
        }

        /* appsandbox-audio.exe — same directory as agent */
        {
            wchar_t audio_src[MAX_PATH], audio_dst[MAX_PATH];
            wcscpy_s(audio_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(audio_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(audio_src, MAX_PATH, L"appsandbox-audio.exe");
            swprintf_s(audio_dst, MAX_PATH, L"%s\\appsandbox-audio.exe", staging);
            if (GetFileAttributesW(audio_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(audio_src, audio_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-audio.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-audio.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-audio.exe not found at %s", audio_src);
            }
        }
    } else {
        ui_log(L"Warning: appsandbox-agent.exe not found");
    }

    /* VDD driver files — copy to drivers\ subdirectory if available */
    {
        wchar_t drivers_staging[MAX_PATH];
        wchar_t vdd_dir[MAX_PATH];
        BOOL found_vdd = FALSE;
        const wchar_t *vdd_files[] = {
            L"AppSandboxVDD.dll", L"AppSandboxVDD.inf",
            L"AppSandboxVDD.cat", L"AppSandboxVDD.cer"
        };
        int vf;
        wchar_t vdd_src[MAX_PATH];

        swprintf_s(drivers_staging, MAX_PATH, L"%s\\drivers", staging);

        if (res_dir) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            wchar_t exe_dir2[MAX_PATH];
            wchar_t *slash2;
            GetModuleFileNameW(NULL, exe_dir2, MAX_PATH);
            slash2 = wcsrchr(exe_dir2, L'\\');
            if (slash2) *slash2 = L'\0';
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", exe_dir2);
            swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
            if (!found_vdd) {
                wcscpy_s(vdd_dir, MAX_PATH, exe_dir2);
                swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
                if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                    found_vdd = TRUE;
            }
        }
        if (found_vdd) {
            CreateDirectoryW(drivers_staging, NULL);
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(vdd_src, MAX_PATH, L"%s\\%s", vdd_dir, vdd_files[vf]);
                swprintf_s(file_path, MAX_PATH, L"%s\\%s", drivers_staging, vdd_files[vf]);
                if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                    CopyFileW(vdd_src, file_path, FALSE);
            }
            /* devcon.exe — alongside VDD driver files */
            swprintf_s(vdd_src, MAX_PATH, L"%s\\devcon.exe", vdd_dir);
            swprintf_s(file_path, MAX_PATH, L"%s\\devcon.exe", drivers_staging);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(vdd_src, file_path, FALSE);
        }
    }

    /* VAD driver files — copy to drivers\ subdirectory if available */
    {
        wchar_t drivers_staging[MAX_PATH];
        wchar_t vad_dir[MAX_PATH];
        BOOL found_vad = FALSE;
        const wchar_t *vad_files[] = {
            L"AppSandboxVAD.sys", L"AppSandboxVAD.inf",
            L"AppSandboxVAD.cat", L"AppSandboxVAD.cer"
        };
        int vf;
        wchar_t vad_src[MAX_PATH];

        swprintf_s(drivers_staging, MAX_PATH, L"%s\\drivers", staging);

        if (res_dir) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            wchar_t exe_dir2[MAX_PATH];
            wchar_t *slash2;
            GetModuleFileNameW(NULL, exe_dir2, MAX_PATH);
            slash2 = wcsrchr(exe_dir2, L'\\');
            if (slash2) *slash2 = L'\0';
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", exe_dir2);
            swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
            if (!found_vad) {
                wcscpy_s(vad_dir, MAX_PATH, exe_dir2);
                swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
                if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                    found_vad = TRUE;
            }
        }
        if (found_vad) {
            CreateDirectoryW(drivers_staging, NULL);
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(vad_src, MAX_PATH, L"%s\\%s", vad_dir, vad_files[vf]);
                swprintf_s(file_path, MAX_PATH, L"%s\\%s", drivers_staging, vad_files[vf]);
                if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                    CopyFileW(vad_src, file_path, FALSE);
            }
        }
    }

    /* SSH MSI — copy to staging root if ssh_enabled */
    if (ssh_enabled) {
        wchar_t msi_path[MAX_PATH];
        if (ensure_ssh_msi_cached(msi_path, MAX_PATH)) {
            swprintf_s(file_path, MAX_PATH, L"%s\\%s", staging, SSH_MSI_NAME);
            if (!CopyFileW(msi_path, file_path, FALSE))
                ui_log(L"Warning: failed to copy SSH MSI (error %lu)", GetLastError());
            else
                ui_log(L"Staged %s for ISO.", SSH_MSI_NAME);
        }
    }

    /* setup.cmd — first-logon script: installs agent service only */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    {
        FILE *cmd;
        if (_wfopen_s(&cmd, file_path, L"w") == 0 && cmd) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === instance setup.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install guest agent\r\n"
                "if exist \"%~dp0appsandbox-agent.exe\" (\r\n"
                "    copy /Y \"%~dp0appsandbox-agent.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-input.exe\" copy /Y \"%~dp0appsandbox-input.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-displays.exe\" copy /Y \"%~dp0appsandbox-displays.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard.exe\" copy /Y \"%~dp0appsandbox-clipboard.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard-reader.exe\" copy /Y \"%~dp0appsandbox-clipboard-reader.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-audio.exe\" copy /Y \"%~dp0appsandbox-audio.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    \"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "echo === instance setup.cmd finished === >> \"%LOG%\"\r\n",
                cmd);
            fclose(cmd);
        }
    }

    /* SetupComplete.cmd — runs as SYSTEM after setup, before first logon.
       Copied to C:\Windows\Setup\Scripts\ during specialize pass.
       Installs VDD driver (cert + test signing + devcon) + optionally OpenSSH. */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    {
        FILE *sc;
        if (_wfopen_s(&sc, file_path, L"w") == 0 && sc) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Find ISO drive\r\n"
                "set ISODRV=\r\n"
                "for %%d in (D E F G H I J) do @if exist %%d:\\drivers\\AppSandboxVDD.inf set ISODRV=%%d:\r\n"
                "if \"%ISODRV%\"==\"\" (\r\n"
                "    echo [VDD] Could not find ISO drive with driver files >> \"%LOG%\"\r\n"
                "    goto :done\r\n"
                ")\r\n"
                "echo [VDD] Found driver files on %ISODRV% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Enable test signing\r\n"
                "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
                "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
                "\r\n"
                "REM Install certificate\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" (\r\n"
                "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
                "    certutil -addstore Root \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                "    certutil -f -addstore TrustedPublisher \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "\r\n"
                "REM Install driver with devcon\r\n"
                "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "dir \"%ISODRV%\\drivers\\\" >> \"%LOG%\" 2>&1\r\n"
                "\"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
                "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Disable display sleep so IDD swap chain stays alive\r\n"
                "powercfg /change monitor-timeout-ac 0\r\n"
                "powercfg /change monitor-timeout-dc 0\r\n"
                "powercfg /change standby-timeout-ac 0\r\n"
                "powercfg /change standby-timeout-dc 0\r\n"
                "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install VAD driver with devcon\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" (\r\n"
                "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "    \"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
                "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                ")\r\n"
                "\r\n",
                sc);

            if (ssh_enabled) {
                fputs(
                    "REM Install OpenSSH Server\r\n"
                    "if exist \"%ISODRV%\\OpenSSH-Win64-v10.0.0.0.msi\" (\r\n"
                    "    echo [SSH] Installing OpenSSH Server... >> \"%LOG%\"\r\n"
                    "    msiexec /i \"%ISODRV%\\OpenSSH-Win64-v10.0.0.0.msi\" /qn /norestart >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] msiexec exit code: %errorlevel% >> \"%LOG%\"\r\n"
                    "    sc config sshd start= auto >> \"%LOG%\" 2>&1\r\n"
                    "    net start sshd >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] Done >> \"%LOG%\"\r\n"
                    ")\r\n"
                    "\r\n",
                    sc);
            }

            fputs(
                ":done\r\n"
                "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
                sc);
            fclose(sc);
        }
    }
}


HRESULT iso_create_instance_resources(const wchar_t *iso_path,
                                       const wchar_t *vm_name,
                                       const wchar_t *admin_user,
                                       wchar_t *admin_pass,
                                       const wchar_t *res_dir,
                                       BOOL ssh_enabled,
                                       const wchar_t *lang)
{
    wchar_t dir[MAX_PATH];
    wchar_t staging[MAX_PATH];
    wchar_t file_path[MAX_PATH];
    wchar_t b64_pass[512];
    wchar_t *last_slash;
    HRESULT hr;

    if (!iso_path || !vm_name || !admin_user || !admin_pass)
        return E_INVALIDARG;

    /* Get output directory from iso_path */
    wcscpy_s(dir, MAX_PATH, iso_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));
        return E_FAIL;
    }
    SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));

    /* Delete stale ISO */
    if (GetFileAttributesW(iso_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(iso_path);

    /* Create staging directory */
    swprintf_s(staging, MAX_PATH, L"%s\\_iso_staging", dir);
    CreateDirectoryW(staging, NULL);

    ui_log(L"Creating instance resources ISO...");

    /* unattend.xml (NOT autounattend.xml — post-sysprep mini-setup uses this name) */
    swprintf_s(file_path, MAX_PATH, L"%s\\unattend.xml", staging);
    if (!generate_unattend_instance(file_path, vm_name, admin_user, b64_pass, lang))
        ui_log(L"Warning: failed to write instance unattend.xml");

    /* Agent exe + setup.cmd + SSH MSI */
    stage_agent_and_setup(staging, res_dir, ssh_enabled);

    /* Build ISO */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = create_iso_from_dir(iso_path, staging, L"APPSETUP");
    CoUninitialize();

    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    remove_staging_dir(staging);

    if (SUCCEEDED(hr))
        ui_log(L"Instance resources ISO created: %s", iso_path);

    return hr;
}

/* ---- VHDX-First VM Creation ---- */

#include "gpu_enum.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

/* Generate unattend.xml for VHDX-first boot.
   No windowsPE pass (DISM already applied the image).
   Only specialize + oobeSystem for first-boot mini-setup. */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode,
                             const wchar_t *lang)
{
    FILE *f;
    wchar_t comp_name[16];
    wchar_t b64_pass[512];

    if (!output_path || !vm_name || !admin_user || !admin_pass)
        return FALSE;

    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        return FALSE;
    }

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f) {
        SecureZeroMemory(b64_pass, sizeof(b64_pass));
        return FALSE;
    }

    /* specialize pass: ComputerName + BypassNRO + optional testsigning */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        comp_name);

    {
        int order = 2;
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            L"                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fwprintf(f,
                L"                <RunSynchronousCommand wcm:action=\"add\">\n"
                L"                    <Order>%d</Order>\n"
                L"                    <Path>bcdedit /set testsigning on</Path>\n"
                L"                </RunSynchronousCommand>\n", order++);
        }
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n");

    /* oobeSystem pass: locale, OOBE hiding, user account, FirstLogonCommand (on-disk path) */
    fwprintf(f,
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <OOBE>\n"
        L"                <HideEULAPage>true</HideEULAPage>\n"
        L"                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
        L"                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
        L"                <ProtectYourPC>3</ProtectYourPC>\n"
        L"            </OOBE>\n"
        L"            <UserAccounts><LocalAccounts>\n"
        L"                <LocalAccount wcm:action=\"add\">\n"
        L"                    <Name>%s</Name>\n"
        L"                    <Group>Administrators</Group>\n"
        L"                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                </LocalAccount>\n"
        L"            </LocalAccounts></UserAccounts>\n"
        L"            <AutoLogon>\n"
        L"                <Enabled>true</Enabled>\n"
        L"                <Username>%s</Username>\n"
        L"                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                <LogonCount>1</LogonCount>\n"
        L"            </AutoLogon>\n"
        L"            <FirstLogonCommands>\n"
        L"                <SynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Description>Run AppSandbox setup</Description>\n"
        L"                    <CommandLine>C:\\Windows\\AppSandbox\\setup.cmd</CommandLine>\n"
        L"                    <RequiresUserInput>false</RequiresUserInput>\n"
        L"                </SynchronousCommand>\n"
        L"            </FirstLogonCommands>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n",
        lang_to_input_locale(lang), lang, lang, lang,
        admin_user, b64_pass,
        admin_user, b64_pass);

    fclose(f);
    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    return TRUE;
}

/* Generate unattend.xml for VHDX-first *template* boot.
   No windowsPE (DISM already applied the image).
   specialize: ComputerName + BypassNRO + testsigning + BitLocker disable
   oobeSystem: Reseal Audit (no user account, no OOBE)
   auditUser:  sysprep /generalize /oobe /shutdown /mode:vm */
BOOL generate_unattend_vhdx_template(const wchar_t *output_path,
                                      const wchar_t *vm_name,
                                      BOOL test_mode)
{
    FILE *f;
    wchar_t comp_name[16];

    if (!output_path || !vm_name)
        return FALSE;

    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    /* specialize pass: ComputerName + BypassNRO + testsigning + BitLocker disable */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        comp_name);

    {
        int order = 2;
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            L"                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fwprintf(f,
                L"                <RunSynchronousCommand wcm:action=\"add\">\n"
                L"                    <Order>%d</Order>\n"
                L"                    <Path>bcdedit /set testsigning on</Path>\n"
                L"                </RunSynchronousCommand>\n", order++);
        }
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <PreventDeviceEncryption>true</PreventDeviceEncryption>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n");

    /* oobeSystem: Reseal Audit mode (no user account, boots into audit) */
    /* auditUser: sysprep /generalize /oobe /shutdown /mode:vm */
    fwprintf(f,
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <Reseal>\n"
        L"                <Mode>Audit</Mode>\n"
        L"            </Reseal>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"auditUser\">\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Description>Generalize VM and shut down for templating</Description>\n"
        L"                    <Path>C:\\Windows\\System32\\Sysprep\\sysprep.exe /generalize /oobe /shutdown /mode:vm</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n");

    fclose(f);
    return TRUE;
}

/* Generate setup.cmd for VHDX-first boot (agent already on disk) */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path)
{
    FILE *cmd;
    if (_wfopen_s(&cmd, output_path, L"w") != 0 || !cmd)
        return FALSE;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "echo === setup.cmd started === >> \"%LOG%\"\r\n"
        "REM Agent already at C:\\Windows\\AppSandbox\\ from VHDX staging\r\n"
        "\"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
        "echo === setup.cmd finished === >> \"%LOG%\"\r\n",
        cmd);
    fclose(cmd);
    return TRUE;
}

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk) */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path, BOOL ssh_enabled)
{
    FILE *sc;
    if (_wfopen_s(&sc, output_path, L"w") != 0 || !sc)
        return FALSE;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
        "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
        "\r\n"
        "set DRVDIR=%SystemRoot%\\AppSandbox\\drivers\r\n"
        "if not exist \"%DRVDIR%\\AppSandboxVDD.inf\" goto :done\r\n"
        "\r\n"
        "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
        "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
        "\r\n"
        "if exist \"%DRVDIR%\\AppSandboxVDD.cer\" (\r\n"
        "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
        "    certutil -addstore Root \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        "    certutil -f -addstore TrustedPublisher \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        ")\r\n"
        "\r\n"
        "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
        "\"%DRVDIR%\\devcon.exe\" install \"%DRVDIR%\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
        "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        "\r\n"
        "REM Disable display sleep so IDD swap chain stays alive\r\n"
        "powercfg /change monitor-timeout-ac 0\r\n"
        "powercfg /change monitor-timeout-dc 0\r\n"
        "powercfg /change standby-timeout-ac 0\r\n"
        "powercfg /change standby-timeout-dc 0\r\n"
        "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
        "\r\n"
        "REM Install VAD driver with devcon\r\n"
        "if exist \"%DRVDIR%\\AppSandboxVAD.inf\" (\r\n"
        "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
        "    \"%DRVDIR%\\devcon.exe\" install \"%DRVDIR%\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
        "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        ")\r\n"
        "\r\n",
        sc);

    if (ssh_enabled) {
        fputs(
            "REM Install OpenSSH Server\r\n"
            "set SSHDIR=%SystemRoot%\\AppSandbox\r\n"
            "if exist \"%SSHDIR%\\OpenSSH-Win64-v10.0.0.0.msi\" (\r\n"
            "    echo [SSH] Installing OpenSSH Server... >> \"%LOG%\"\r\n"
            "    msiexec /i \"%SSHDIR%\\OpenSSH-Win64-v10.0.0.0.msi\" /qn /norestart >> \"%LOG%\" 2>&1\r\n"
            "    echo [SSH] msiexec exit code: %errorlevel% >> \"%LOG%\"\r\n"
            "    sc config sshd start= auto >> \"%LOG%\" 2>&1\r\n"
            "    net start sshd >> \"%LOG%\" 2>&1\r\n"
            "    echo [SSH] Done >> \"%LOG%\"\r\n"
            ")\r\n"
            "\r\n",
            sc);
    }

    fputs(
        ":done\r\n"
        "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
        sc);
    fclose(sc);
    return TRUE;
}

/* Find the exe directory (directory containing AppSandbox.exe) */
static void get_exe_dir(wchar_t *out, size_t out_len)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, out, (DWORD)out_len);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

/* Recursively enumerate files in a directory matching semicolon-separated glob filters.
   Writes matching files as manifest entries to the file handle.
   host_prefix: the host-side base path (e.g. C:\Windows\System32\DriverStore\...).
   guest_prefix: the guest-side base path (e.g. \Windows\System32\HostDriverStore\...).
   filter: semicolon-separated patterns (e.g. "*.dll;*.sys;nv*.inf") or empty for all.
   Returns the number of entries written. */
static int enumerate_gpu_share_files(FILE *mf,
                                      const wchar_t *host_prefix,
                                      const wchar_t *guest_prefix,
                                      const wchar_t *filter)
{
    wchar_t pattern[MAX_PATH], host_full[MAX_PATH], guest_full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", host_prefix);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;

        swprintf_s(host_full, MAX_PATH, L"%s\\%s", host_prefix, fd.cFileName);
        swprintf_s(guest_full, MAX_PATH, L"%s\\%s", guest_prefix, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += enumerate_gpu_share_files(mf, host_full, guest_full, filter);
        } else {
            /* Check filter: if filter is empty, accept all; otherwise check each pattern */
            BOOL match = FALSE;
            if (!filter || filter[0] == L'\0') {
                match = TRUE;
            } else {
                /* PathMatchSpecW supports semicolon-separated patterns natively */
                match = PathMatchSpecW(fd.cFileName, filter);
            }
            if (match) {
                fwprintf(mf, L"%s\t%s\n", host_full, guest_full);
                count++;
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return count;
}

/* Generate staging manifest for iso-patch --stage.
   Writes tab-separated source\tdest lines for all files to pre-stage on the VHDX. */
int generate_vhdx_manifest(const wchar_t *manifest_path,
                            const wchar_t *staging_dir,
                            const wchar_t *res_dir,
                            const void *gpu_shares_ptr,
                            BOOL ssh_enabled)
{
    const GpuDriverShareList *gpu_shares = (const GpuDriverShareList *)gpu_shares_ptr;
    FILE *mf;
    int count = 0;
    wchar_t src[MAX_PATH], exe_dir[MAX_PATH];

    if (_wfopen_s(&mf, manifest_path, L"w,ccs=UTF-8") != 0 || !mf)
        return -1;

    get_exe_dir(exe_dir, MAX_PATH);

    /* 1. Staging files: unattend.xml, setup.cmd, SetupComplete.cmd */
    swprintf_s(src, MAX_PATH, L"%s\\unattend.xml", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\Panther\\unattend.xml\n", src);
        count++;
    }

    swprintf_s(src, MAX_PATH, L"%s\\setup.cmd", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\setup.cmd\n", src);
        count++;
    }

    swprintf_s(src, MAX_PATH, L"%s\\SetupComplete.cmd", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\Setup\\Scripts\\SetupComplete.cmd\n", src);
        count++;
    }

    /* 2. Agent + input helper executables */
    {
        const wchar_t *bins[] = { L"appsandbox-agent.exe", L"appsandbox-input.exe", L"appsandbox-displays.exe", L"appsandbox-clipboard.exe", L"appsandbox-clipboard-reader.exe", L"appsandbox-audio.exe" };
        int bi;
        for (bi = 0; bi < (int)(sizeof(bins) / sizeof(bins[0])); bi++) {
            BOOL found = FALSE;
            if (res_dir) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", res_dir, bins[bi]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                    found = TRUE;
            }
            if (!found) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", exe_dir, bins[bi]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                    found = TRUE;
            }
            if (found) {
                fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\%s\n", src, bins[bi]);
                count++;
            }
        }
    }

    /* 3. VDD driver files */
    {
        const wchar_t *vdd_files[] = {
            L"AppSandboxVDD.dll", L"AppSandboxVDD.inf",
            L"AppSandboxVDD.cat", L"AppSandboxVDD.cer", L"devcon.exe"
        };
        wchar_t vdd_dir[MAX_PATH];
        BOOL found_vdd = FALSE;
        int vf;

        /* Search for VDD files in res_dir\drivers, exe_dir\drivers, or exe_dir */
        if (res_dir) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            wcscpy_s(vdd_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }

        if (found_vdd) {
            for (vf = 0; vf < 5; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", vdd_dir, vdd_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, vdd_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3a. VAD driver files */
    {
        const wchar_t *vad_files[] = {
            L"AppSandboxVAD.sys", L"AppSandboxVAD.inf",
            L"AppSandboxVAD.cat", L"AppSandboxVAD.cer"
        };
        wchar_t vad_dir[MAX_PATH];
        BOOL found_vad = FALSE;
        int vf;

        if (res_dir) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            wcscpy_s(vad_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }

        if (found_vad) {
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", vad_dir, vad_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, vad_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3b. SSH MSI */
    if (ssh_enabled) {
        wchar_t msi_path[MAX_PATH];
        if (ensure_ssh_msi_cached(msi_path, MAX_PATH)) {
            fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\%s\n", msi_path, SSH_MSI_NAME);
            count++;
        }
    }

    /* 4. GPU driver files (one line per file, enumerated from host DriverStore paths).
       guest_path from GpuDriverShare is absolute (e.g. "C:\Windows\System32\HostDriverStore\...").
       The manifest dest must be relative to the VHDX root (e.g. "\Windows\...").
       Strip the drive letter prefix ("C:") to make it relative. */
    if (gpu_shares && gpu_shares->count > 0) {
        int i;
        for (i = 0; i < gpu_shares->count; i++) {
            const GpuDriverShare *ds = &gpu_shares->shares[i];
            const wchar_t *rel_guest = ds->guest_path;
            if (GetFileAttributesW(ds->host_path) == INVALID_FILE_ATTRIBUTES)
                continue;
            /* Strip "C:" or any drive prefix — keep the leading backslash */
            if (rel_guest[0] != L'\0' && rel_guest[1] == L':')
                rel_guest += 2;
            count += enumerate_gpu_share_files(mf, ds->host_path, rel_guest, ds->file_filter);
        }
    }

    fclose(mf);
    return count;
}

/* ---- Language JSON helpers ---- */

/* Derive language.json path from VHDX path (same directory) */
static void get_language_json_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_len)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_len, L"%s\\language.json", dir);
}

void vm_save_language_json(const wchar_t *vhdx_path, const wchar_t *lang)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char narrow[64];
    get_language_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    WideCharToMultiByte(CP_UTF8, 0, lang, -1, narrow, sizeof(narrow), NULL, NULL);
    fprintf(f, "{\"language\":\"%s\"}\n", narrow);
    fclose(f);
}

BOOL vm_load_language_json(const wchar_t *vhdx_path, wchar_t *lang_out, int lang_out_max)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    get_language_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return FALSE;
    if (fgets(buf, sizeof(buf), f)) {
        /* Parse {"language":"xx-YY"} */
        char *p = strstr(buf, "\"language\":\"");
        if (p) {
            char *start, *end;
            p += 12;  /* skip past "language":" */
            start = p;
            end = strchr(start, '"');
            if (end) {
                *end = '\0';
                MultiByteToWideChar(CP_UTF8, 0, start, -1, lang_out, lang_out_max);
                fclose(f);
                return TRUE;
            }
        }
    }
    fclose(f);
    return FALSE;
}

/* ============================================================================
 *  Ubuntu / Linux guest support: cidata.iso (NoCloud) + autoinstall
 * ============================================================================
 *
 *  The ISO this builds is mounted as a second CDROM in the VM. It serves two
 *  jobs at once:
 *
 *    1. cloud-init NoCloud datasource — at the ISO root, `user-data` (the
 *       autoinstall YAML) and `meta-data` (instance-id + hostname). The
 *       subiquity installer in the main Ubuntu ISO finds this by the volume
 *       label "cidata" and runs autoinstall.
 *
 *    2. Resource staging — the `extras/` subtree contains everything that
 *       has to land in the installed system: agent ELFs, dxgkrnl kernel
 *       module (prebuilt + DKMS source), asb_drm, wsl-mesa tarball, wsl-deps
 *       proprietary D3D libs, systemd units, and `setup.sh`. The autoinstall
 *       `late-commands` hook mounts the cidata volume from inside the
 *       installer environment, copies `extras/*` into /target/opt/appsandbox/,
 *       then `curtin in-target` runs `setup.sh` inside the new system's
 *       chroot to wire up systemd units, DKMS, ld.so.conf.d, and so on.
 * ========================================================================== */

/* ---- SHA-512 crypt (glibc $6$ format) ----
 *
 * Linux passwd/shadow stores hashed passwords in crypt(3) format. The
 * autoinstall `identity.password` field needs a string in this format —
 * plain text isn't accepted. We can't shell out to `mkpasswd` (not present
 * on Windows), and OpenSSL isn't guaranteed. So implement Ulrich Drepper's
 * SHA-512 crypt algorithm on top of Win32 CNG's SHA-512 primitive.
 *
 * Reference: https://www.akkadia.org/drepper/sha-crypt.html
 *            https://www.akkadia.org/drepper/SHA-crypt.txt
 */

#define SHA512_DIGEST_LEN 64

typedef struct {
    const void *data;
    ULONG       len;
} sha512_part_t;

/* One-shot SHA-512: hash a list of (data, len) parts and write the 64-byte
   digest to `out`. Returns TRUE on success. */
static BOOL sha512_oneshot(const sha512_part_t *parts,
                            int num_parts, BYTE out[SHA512_DIGEST_LEN])
{
    BCRYPT_ALG_HANDLE  hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status;
    BOOL ok = FALSE;
    int i;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0);
    if (status != 0) return FALSE;

    status = BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
    if (status != 0) goto done;

    for (i = 0; i < num_parts; i++) {
        if (parts[i].len == 0) continue;
        status = BCryptHashData(hHash, (PUCHAR)parts[i].data, parts[i].len, 0);
        if (status != 0) goto done;
    }

    status = BCryptFinishHash(hHash, out, SHA512_DIGEST_LEN, 0);
    if (status == 0) ok = TRUE;

done:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

/* GNU crypt b64 alphabet — note "./" prefix and the rest is digits, upper, lower. */
static const char crypt_b64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Encode three bytes as four crypt-b64 chars, in the specific order Drepper
   defined. n = number of chars to emit (1..4). */
static void crypt_b64_3(unsigned b2, unsigned b1, unsigned b0, int n, char **out)
{
    unsigned w = (b2 << 16) | (b1 << 8) | b0;
    while (n-- > 0) {
        *(*out)++ = crypt_b64[w & 0x3f];
        w >>= 6;
    }
}

/* Drepper SHA-512 crypt. `key` is the password bytes; `salt` is up to 16
   bytes from the crypt_b64 alphabet. Writes the encoded result (including
   the "$6$<salt>$" prefix) into `out`. `out_size` must be >= 110.
   Returns the number of chars written (not including NUL), or 0 on error. */
static int sha512_crypt(const char *key, size_t key_len,
                         const char *salt, size_t salt_len,
                         char *out, size_t out_size)
{
    /* Algorithm uses two "rolling" SHA-512 inputs: digest A (the per-round
       hash) and digest B (a key-derived seed used to padding-extend A). */
    BYTE alt[SHA512_DIGEST_LEN];   /* digest B (intermediate) */
    BYTE digest[SHA512_DIGEST_LEN]; /* digest A / running hash C */
    BYTE p_seq[SHA512_DIGEST_LEN]; /* DP digest, expanded to P below */
    BYTE s_seq[SHA512_DIGEST_LEN]; /* DS digest, expanded to S below */
    BYTE *P = NULL, *S = NULL;
    int rc = 0;
    size_t cnt;
    int rounds = 5000;  /* default rounds for $6$ */
    char *cp;
    int i;

    sha512_part_t parts[64];
    int n;

    if (key_len > 256 || salt_len > 16 || out_size < 110)
        return 0;

    /* --- Step 4-8: compute digest B = SHA512(key || salt || key) --- */
    n = 0;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    parts[n].data = salt; parts[n].len = (ULONG)salt_len; n++;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    if (!sha512_oneshot(parts, n, alt)) return 0;

    /* --- Step 1-3, 9-12: digest A = SHA512(key, salt, then key-len bytes
       from alt (full 64-byte blocks at a time), then key-bit-pattern feeds.) */
    n = 0;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    parts[n].data = salt; parts[n].len = (ULONG)salt_len; n++;

    /* Feed full 64-byte blocks of alt for each 64-byte chunk of key length */
    cnt = key_len;
    while (cnt > SHA512_DIGEST_LEN) {
        parts[n].data = alt; parts[n].len = SHA512_DIGEST_LEN; n++;
        cnt -= SHA512_DIGEST_LEN;
    }
    if (cnt > 0) {
        parts[n].data = alt; parts[n].len = (ULONG)cnt; n++;
    }

    /* For each bit of key_len (low bit first), alternately feed alt or key */
    for (cnt = key_len; cnt > 0; cnt >>= 1) {
        if (cnt & 1) {
            parts[n].data = alt; parts[n].len = SHA512_DIGEST_LEN; n++;
        } else {
            parts[n].data = key; parts[n].len = (ULONG)key_len; n++;
        }
    }

    if (!sha512_oneshot(parts, n, digest)) return 0;

    /* --- Steps 13-15: DP = SHA512(key repeated key_len times). Number of
       feeds equals key length, which can be larger than parts[]. Use
       incremental hashing so we don't care about parts[] capacity here. */
    {
        BCRYPT_ALG_HANDLE  hAlg = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;
        NTSTATUS status;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0) != 0) goto cleanup;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0); goto cleanup;
        }
        for (cnt = 0; cnt < key_len; cnt++) {
            status = BCryptHashData(hHash, (PUCHAR)key, (ULONG)key_len, 0);
            if (status != 0) {
                BCryptDestroyHash(hHash);
                BCryptCloseAlgorithmProvider(hAlg, 0);
                goto cleanup;
            }
        }
        status = BCryptFinishHash(hHash, p_seq, SHA512_DIGEST_LEN, 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (status != 0) goto cleanup;
    }

    /* Expand DP into P: floor(key_len / 64) full copies + (key_len % 64) bytes */
    P = (BYTE *)malloc(key_len > 0 ? key_len : 1);
    if (!P) return 0;
    {
        size_t left = key_len;
        BYTE *dst = P;
        while (left >= SHA512_DIGEST_LEN) {
            memcpy(dst, p_seq, SHA512_DIGEST_LEN);
            dst += SHA512_DIGEST_LEN;
            left -= SHA512_DIGEST_LEN;
        }
        if (left) memcpy(dst, p_seq, left);
    }

    /* --- Steps 16-18: DS = SHA512(salt repeated (16 + digest[0]) times).
       digest[0] is 0..255 so reps can be up to 271 — way past parts[]
       capacity, so incremental again. */
    {
        BCRYPT_ALG_HANDLE  hAlg = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;
        NTSTATUS status;
        int reps = 16 + digest[0];

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0) != 0) goto cleanup;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            goto cleanup;
        }
        for (i = 0; i < reps; i++) {
            status = BCryptHashData(hHash, (PUCHAR)salt, (ULONG)salt_len, 0);
            if (status != 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); goto cleanup; }
        }
        status = BCryptFinishHash(hHash, s_seq, SHA512_DIGEST_LEN, 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (status != 0) goto cleanup;
    }

    /* Expand DS into S */
    S = (BYTE *)malloc(salt_len > 0 ? salt_len : 1);
    if (!S) goto cleanup;
    {
        size_t left = salt_len;
        BYTE *dst = S;
        while (left >= SHA512_DIGEST_LEN) {
            memcpy(dst, s_seq, SHA512_DIGEST_LEN);
            dst += SHA512_DIGEST_LEN;
            left -= SHA512_DIGEST_LEN;
        }
        if (left) memcpy(dst, s_seq, left);
    }

    /* --- Step 21: main loop, 5000 rounds. Each round hashes a sequence of
       (P, S, prev_digest) chosen by the round index parity vs 2/3/7. */
    for (i = 0; i < rounds; i++) {
        n = 0;
        if (i & 1) { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        else       { parts[n].data = digest; parts[n].len = SHA512_DIGEST_LEN; n++; }
        if (i % 3) { parts[n].data = S; parts[n].len = (ULONG)salt_len; n++; }
        if (i % 7) { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        if (i & 1) { parts[n].data = digest; parts[n].len = SHA512_DIGEST_LEN; n++; }
        else       { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        if (!sha512_oneshot(parts, n, digest)) goto cleanup;
    }

    /* --- Step 22: encode digest into crypt_b64 form, in a specific 21-group
       permutation defined by the spec. Final char encodes the leftover byte. */
    cp = out;
    cp += sprintf_s(cp, out_size, "$6$");
    if (salt_len > 0) {
        memcpy(cp, salt, salt_len);
        cp += salt_len;
    }
    *cp++ = '$';

    crypt_b64_3(digest[ 0], digest[21], digest[42], 4, &cp);
    crypt_b64_3(digest[22], digest[43], digest[ 1], 4, &cp);
    crypt_b64_3(digest[44], digest[ 2], digest[23], 4, &cp);
    crypt_b64_3(digest[ 3], digest[24], digest[45], 4, &cp);
    crypt_b64_3(digest[25], digest[46], digest[ 4], 4, &cp);
    crypt_b64_3(digest[47], digest[ 5], digest[26], 4, &cp);
    crypt_b64_3(digest[ 6], digest[27], digest[48], 4, &cp);
    crypt_b64_3(digest[28], digest[49], digest[ 7], 4, &cp);
    crypt_b64_3(digest[50], digest[ 8], digest[29], 4, &cp);
    crypt_b64_3(digest[ 9], digest[30], digest[51], 4, &cp);
    crypt_b64_3(digest[31], digest[52], digest[10], 4, &cp);
    crypt_b64_3(digest[53], digest[11], digest[32], 4, &cp);
    crypt_b64_3(digest[12], digest[33], digest[54], 4, &cp);
    crypt_b64_3(digest[34], digest[55], digest[13], 4, &cp);
    crypt_b64_3(digest[56], digest[14], digest[35], 4, &cp);
    crypt_b64_3(digest[15], digest[36], digest[57], 4, &cp);
    crypt_b64_3(digest[37], digest[58], digest[16], 4, &cp);
    crypt_b64_3(digest[59], digest[17], digest[38], 4, &cp);
    crypt_b64_3(digest[18], digest[39], digest[60], 4, &cp);
    crypt_b64_3(digest[40], digest[61], digest[19], 4, &cp);
    crypt_b64_3(digest[62], digest[20], digest[41], 4, &cp);
    crypt_b64_3(       0,         0, digest[63], 2, &cp);

    *cp = '\0';
    rc = (int)(cp - out);

cleanup:
    if (P) { SecureZeroMemory(P, key_len); free(P); }
    if (S) { SecureZeroMemory(S, salt_len); free(S); }
    SecureZeroMemory(digest, sizeof(digest));
    SecureZeroMemory(alt, sizeof(alt));
    SecureZeroMemory(p_seq, sizeof(p_seq));
    SecureZeroMemory(s_seq, sizeof(s_seq));
    return rc;
}

/* Public-ish wrapper: hash a wide-char password into glibc $6$ format.
   Generates a fresh random 16-char salt. Wipes the UTF-8 conversion of the
   password from memory after use. Returns TRUE on success. */
static BOOL unix_password_hash(const wchar_t *plain, char *hash_out, size_t hash_out_size)
{
    char utf8[512];
    int  utf8_len;
    char salt[17];
    BYTE salt_raw[12];
    int  i, rc;

    if (!plain || !hash_out || hash_out_size < 110) return FALSE;

    utf8_len = WideCharToMultiByte(CP_UTF8, 0, plain, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (utf8_len <= 0) return FALSE;
    /* WideCharToMultiByte includes the NUL in the count when input is
       NUL-terminated. Trim it for the hash input. */
    if (utf8[utf8_len - 1] == '\0') utf8_len--;

    /* 16-char crypt_b64 salt from 12 random bytes (96 bits of entropy). */
    if (BCryptGenRandom(NULL, salt_raw, sizeof(salt_raw),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        SecureZeroMemory(utf8, sizeof(utf8));
        return FALSE;
    }
    for (i = 0; i < 16; i++) {
        /* Pack 3 random bytes -> 4 b64 chars, take 16 of them. */
        unsigned b = salt_raw[i % sizeof(salt_raw)];
        salt[i] = crypt_b64[b & 0x3f];
    }
    salt[16] = '\0';

    rc = sha512_crypt(utf8, (size_t)utf8_len, salt, 16, hash_out, hash_out_size);
    SecureZeroMemory(utf8, sizeof(utf8));
    SecureZeroMemory(salt_raw, sizeof(salt_raw));
    return rc > 0;
}

/* ---- YAML / shell generators ---- */

/* Write `user-data` — plain cloud-init (no autoinstall: envelope).
 *
 * Two-stage boot, mirroring the Windows resources.iso pattern:
 *
 *   Stage 1 (this file, cloud-init bootcmd) — runs in cloud-init.service
 *   BEFORE network-online.target. No internet needed. We:
 *     - Configure hostname + admin user account (declarative, no network)
 *     - Mount the cidata ISO, copy /extras/ -> /opt/appsandbox/
 *     - Install the agent ELF + appsandbox-agent.service +
 *       appsandbox-firstboot.service to system locations
 *     - Touch /etc/appsandbox-firstboot.marker (firstboot service guard)
 *     - systemctl enable --now appsandbox-agent.service so it starts
 *       immediately and connects to the host via vsock
 *
 *   Stage 2 (appsandbox-firstboot.service -> /opt/appsandbox/setup.sh) —
 *   runs AFTER the agent has applied set_ip and network-online.target
 *   has fired. Does all network-dependent work: apt install desktop,
 *   apt install openssh-server (if requested), install rest of agent
 *   service family, install dxgkrnl + asb_drm modules, extract wsl-mesa,
 *   set graphical.target, reboot.
 *
 * Crucially: NO `runcmd:`, NO `package_update`, NO `packages:`. Anything
 * in those blocks would block cloud-final.service on network-online,
 * which won't fire until the agent applies the host-assigned static IP —
 * chicken/egg. bootcmd alone gets the agent up early; the rest is the
 * firstboot service's responsibility.
 *
 * Maps to Windows pattern:
 *   bootcmd      ~ setup.cmd  (first-boot install, no network)
 *   setup.sh     ~ SetupComplete.cmd (drivers, network-dependent work)
 *   firstboot   .marker  ~ FirstLogonCommand guard (runs once)
 */
static BOOL generate_autoinstall_userdata(const wchar_t *path,
                                           const wchar_t *vm_name,
                                           const wchar_t *admin_user,
                                           const char *pw_hash,
                                           BOOL ssh_enabled)
{
    FILE *f;
    char hostname[64], username[128];

    /* Binary mode ("wb") not text mode ("w"). On Windows, "w" translates
       every "\n" → "\r\n" via the CRT, which corrupts every file destined
       for Linux. cloud-init's YAML parser tolerates CRLF but `setup.sh`'s
       `#!/bin/bash` shebang does NOT — kernel execve looks for an
       interpreter literally named "/bin/bash\r" and returns ENOENT
       (systemd 203/EXEC: "No such file or directory"). Same trap for
       network-config and meta-data; not Linux-fatal but still wrong. */
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return FALSE;

    WideCharToMultiByte(CP_UTF8, 0, vm_name, -1, hostname, sizeof(hostname), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, admin_user, -1, username, sizeof(username), NULL, NULL);

    fprintf(f,
        "#cloud-config\n"
        "# Subiquity autoinstall config (Ubuntu 22.04+, Desktop ISO 23.10+).\n"
        "# Subiquity reads this on boot of the Ubuntu Desktop installer ISO,\n"
        "# runs a fully-unattended install to /dev/sda, then reboots into the\n"
        "# installed system where the nested user-data block below applies\n"
        "# our cloud-init config (bootcmd, agent install, etc.).\n"
        "autoinstall:\n"
        "  version: 1\n"
        "  # Don't try to update subiquity itself — we may not have internet\n"
        "  # before the network section below brings eth0 up.\n"
        "  refresh-installer:\n"
        "    update: false\n"
        "  # Empty list = no installer prompts; runs straight through.\n"
        "  interactive-sections: []\n"
        "  locale: en_US.UTF-8\n"
        "  keyboard:\n"
        "    layout: us\n"
        "  # The Desktop ISO's package set already includes\n"
        "  # ubuntu-desktop-minimal + GDM, so we don't add it here.\n"
        "  identity:\n"
        "    realname: %s\n"
        "    hostname: %s\n"
        "    username: %s\n"
        "    password: '%s'\n"
        "  ssh:\n"
        "    install-server: %s\n"
        "    allow-pw: true\n"
        "  # Use whole disk, default partition layout.\n"
        "  storage:\n"
        "    layout:\n"
        "      name: direct\n"
        "  # Extra apt packages installed during the regular install phase\n"
        "  # (subiquity finishes this BEFORE late-commands runs). We need:\n"
        "  #   - dkms + build-essential + linux-headers-generic to build\n"
        "  #     our out-of-tree kernel modules (asb_drm, dxgkrnl) against\n"
        "  #     whatever kernel the ISO is currently shipping.\n"
        "  packages:\n"
        "    - dkms\n"
        "    - build-essential\n"
        "    - linux-headers-generic\n"
        "    - zstd\n"
        "  # Default updates: security (subiquity's default). May bump kernel\n"
        "  # from ISO baseline mid-install; our late-commands DKMS loop\n"
        "  # iterates every /target/lib/modules/<kver>/ so all installed\n"
        "  # kernels end up with our .ko's built and ready.\n"
        "  updates: security\n"
        "  # late-commands run inside the installer environment with the\n"
        "  # installed system mounted at /target, just before reboot. By\n"
        "  # this point packages above are already installed in /target.\n"
        "  # Subiquity reference says cidata is at LABEL=cidata.\n"
        "  late-commands:\n"
        "    - mkdir -p /tmp/cidata\n"
        "    - dev=$(blkid -L cidata) && mount -o ro \"$dev\" /tmp/cidata\n"
        "    - mkdir -p /target/opt/appsandbox\n"
        "    - cp -a /tmp/cidata/extras/. /target/opt/appsandbox/\n"
        "    - chmod +x /target/opt/appsandbox/setup.sh\n"
        "    # 5 ELFs in /usr/local/bin (agent + 4 helpers).\n"
        "    - install -m 0755 /target/opt/appsandbox/appsandbox-agent      /target/usr/local/bin/\n"
        "    - install -m 0755 /target/opt/appsandbox/appsandbox-audio      /target/usr/local/bin/\n"
        "    - install -m 0755 /target/opt/appsandbox/appsandbox-clipboard  /target/usr/local/bin/\n"
        "    - install -m 0755 /target/opt/appsandbox/appsandbox-display    /target/usr/local/bin/\n"
        "    - install -m 0755 /target/opt/appsandbox/appsandbox-input      /target/usr/local/bin/\n"
        "    # 4 system .service files + firstboot.service.\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-agent.service     /target/etc/systemd/system/\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-audio.service     /target/etc/systemd/system/\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-display.service   /target/etc/systemd/system/\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-input.service     /target/etc/systemd/system/\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-firstboot.service /target/etc/systemd/system/\n"
        "    # Unbinds simple-framebuffer.0 before display-manager so mutter\n"
        "    # only sees asb_drm. `video=efifb:off video=simplefb:off` (set\n"
        "    # below) does NOT prevent simpledrm from binding on Hyper-V Gen 2\n"
        "    # because simpledrm is a built-in driver and Hyper-V's UEFI screen_info\n"
        "    # path creates the platform device via sysfb, not efifb.\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/asb-evict-simpledrm.service /target/etc/systemd/system/\n"
        "    # Clipboard is a user-level unit (graphical-session.target).\n"
        "    - install -d /target/etc/systemd/user\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/appsandbox-clipboard.service /target/etc/systemd/user/\n"
        "    # modules-load.d + modprobe.d for asb_drm and snd-aloop.\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/modules-load.d-asb_drm.conf   /target/etc/modules-load.d/asb_drm.conf\n"
        "    - install -m 0644 /target/opt/appsandbox/systemd/modules-load.d-snd-aloop.conf /target/etc/modules-load.d/snd-aloop.conf\n"
        "    - install -m 0644 /target/opt/appsandbox/modprobe.d-asb_drm.conf               /target/etc/modprobe.d/asb_drm.conf\n"
        "    # JIT-build kernel modules against the target's kernel. dkms +\n"
        "    # build-essential + linux-headers-generic were apt-installed above\n"
        "    # by autoinstall.packages, so the toolchain is ready in /target.\n"
        "    - install -d /target/usr/src\n"
        "    - cp -a /target/opt/appsandbox/dxgkrnl-src /target/usr/src/dxgkrnl-2.0.3\n"
        "    - cp -a /target/opt/appsandbox/asb_drm-src /target/usr/src/asb_drm-1.0.0\n"
        "    - curtin in-target --target=/target -- dkms add -m dxgkrnl/2.0.3\n"
        "    - curtin in-target --target=/target -- dkms add -m asb_drm/1.0.0\n"
        "    # Build for every kernel in the target's /lib/modules — subiquity's\n"
        "    # default 'updates: security' commonly pulls a newer kernel mid-\n"
        "    # install (e.g. ISO ships -14, security update bumps to -15), so\n"
        "    # building only for `uname -r` (the installer's kernel) leaves\n"
        "    # the post-reboot system without modules. Iterating /lib/modules\n"
        "    # covers every kernel that will be bootable. Wrap in a bash block\n"
        "    # that dumps every make.log to stdout on failure — subiquity\n"
        "    # captures stdout into its event log, so a build error becomes\n"
        "    # visible in the failure dialog instead of hidden behind a\n"
        "    # generic 'Bad return status'.\n"
        "    - curtin in-target --target=/target -- bash -c 'set -e; for K in $(ls /lib/modules); do echo \"=== dkms autoinstall for $K ===\"; dkms autoinstall -k \"$K\" || { echo \"=== build failed for $K; dumping make.log(s) ===\"; for log in $(find /var/lib/dkms -name make.log 2>/dev/null); do echo \"---- $log ----\"; cat \"$log\"; done; exit 1; }; done'\n"
        "    # Defense in depth: suppress legacy efifb/simplefb fbdev paths.\n"
        "    # NOTE these cmdline knobs do NOT stop simpledrm on Hyper-V Gen 2,\n"
        "    # because simpledrm binds to the sysfb-created simple-framebuffer.0\n"
        "    # platform device, not to efifb/simplefb. Killing the live binding\n"
        "    # is asb-evict-simpledrm.service's job (enabled below).\n"
        "    - install -d /target/etc/default/grub.d\n"
        "    - printf 'GRUB_CMDLINE_LINUX_DEFAULT=\"$GRUB_CMDLINE_LINUX_DEFAULT video=efifb:off video=simplefb:off\"\\n' > /target/etc/default/grub.d/99-appsandbox-no-efifb.cfg\n"
        "    - curtin in-target --target=/target -- update-grub\n"
        "    # firstboot marker — setup.sh still handles wsl-mesa + wsl-deps.\n"
        "    - touch /target/etc/appsandbox-firstboot.marker\n"
        "    # Enable services so they start on first boot.\n"
        "    - curtin in-target --target=/target -- systemctl daemon-reload\n"
        "    - curtin in-target --target=/target -- systemctl enable appsandbox-agent.service appsandbox-audio.service appsandbox-display.service appsandbox-input.service appsandbox-firstboot.service asb-evict-simpledrm.service\n"
        "    # NOTE: appsandbox-clipboard is NOT a user-systemd unit. It's\n"
        "    # spawned by the system-level appsandbox-agent's monitor thread\n"
        "    # (see clipboard-agent-spawn-pattern memory) so it can pick up\n"
        "    # the Mutter XAUTHORITY cookie and the privileged vsock fds.\n"
        "    # wsl-mesa: custom Mesa with d3d12 gallium + dzn Vulkan, installed\n"
        "    # parallel to /opt/wsl-mesa so stock Ubuntu Mesa stays intact.\n"
        "    # Tarball is staged in cidata extras; absent on no-GPU-only builds.\n"
        "    - if [ -f /tmp/cidata/extras/wsl-mesa.tar.zst ]; then zstd -d /tmp/cidata/extras/wsl-mesa.tar.zst -c | tar -C /target -x; fi\n"
        "    # systemd-user-environment-generator + gnome-shell drop-in:\n"
        "    # together they expose Mesa-d3d12 to every .desktop app via the\n"
        "    # user@1000.service environment, but keep mutter on llvmpipe\n"
        "    # (the d3d12 gallium pipe has no GBM compatibility with asb_drm).\n"
        "    - install -m 0755 -D /tmp/cidata/extras/50-appsandbox-gpu /target/etc/systemd/user-environment-generators/50-appsandbox-gpu\n"
        "    - install -m 0644 -D /tmp/cidata/extras/org.gnome.Shell-no-gpu.conf /target/etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf\n"
        "    # appsandbox-gpu wrapper script — opt-in command-line GPU\n"
        "    # acceleration for cases where the user wants to run a specific\n"
        "    # command via host GPU (e.g. from an SSH shell).\n"
        "    - install -m 0755 /tmp/cidata/extras/appsandbox-gpu /target/usr/local/bin/appsandbox-gpu\n"
        "    - umount /tmp/cidata\n"
        "  # Cloud-init applied to the installed system's first boot.\n"
        "  user-data:\n"
        "    # Tee every cloud-init module's stdout/stderr to /dev/console\n"
        "    # (=ttyS0 given our debug-serial kernel cmdline) so a stall in\n"
        "    # any module is visible on the Hyper-V COM1 pipe.\n"
        "    output:\n"
        "      all: '| tee -a /dev/console'\n"
        "    hostname: %s\n"
        "    manage_etc_hosts: true\n"
        "    preserve_hostname: false\n"
        "    users:\n"
        "      - default\n"
        "      - name: %s\n"
        "        gecos: %s\n"
        "        sudo: ALL=(ALL) NOPASSWD:ALL\n"
        "        groups: [adm, sudo, audio, video, plugdev, dialout]\n"
        "        shell: /bin/bash\n"
        "        lock_passwd: false\n"
        "        passwd: '%s'\n"
        "    ssh_pwauth: %s\n"
        "    chpasswd:\n"
        "      expire: false\n"
        "    # bootcmd on first boot of installed system. Agent + helpers\n"
        "    # are already installed and enabled by late-commands; this\n"
        "    # block only handles debug visibility + the optional ssh\n"
        "    # marker so setup.sh knows whether to install openssh-server.\n"
        "    bootcmd:\n"
        "      # Background watchdog every 2s — dumps running cloud-init/\n"
        "      # netplan/ssh procs + last 4 KB of cloud-init.log to ttyS0\n"
        "      # via systemd-run so it survives bootcmd's process group exit.\n"
        "      - [ sh, -c, \"systemctl is-active --quiet asb-debug-watch.service || systemd-run --no-block --unit=asb-debug-watch --description='AppSandbox boot watchdog' /bin/sh -c 'while :; do { echo \\\"--- $(date +%%T) cloud-init-network watchdog ---\\\"; ps -eo pid,ppid,etime,cmd | grep -E \\\"cloud-init|netplan|ssh-keygen|systemd-resolve\\\" | grep -v grep; echo \\\"-- tail cloud-init.log --\\\"; tail -c 4096 /var/log/cloud-init.log 2>/dev/null; echo; } > /dev/console 2>&1; sleep 2; done'\" ]\n"
        "      - [ sh, -c, \"mkdir -p /var/log/appsandbox && echo bootcmd $(date -u) >> /var/log/appsandbox/cidata.log\" ]\n"
        "%s",
        /* autoinstall.identity */
        username,                                          /* realname (display name on GDM) */
        hostname,                                          /* hostname */
        username,                                          /* username */
        pw_hash,                                           /* password */
        ssh_enabled ? "true" : "false",                    /* ssh.install-server */
        /* autoinstall.user-data — nested cloud-config for installed-system first boot */
        hostname,                                          /* hostname */
        username, username,                                /* user name + gecos */
        pw_hash,                                           /* passwd */
        ssh_enabled ? "true" : "false",                    /* ssh_pwauth */
        /* Conditional ssh marker entry inside bootcmd. Indentation matches
           the surrounding `bootcmd:` items (6 spaces + "- "). */
        ssh_enabled
            ? "      - [ sh, -c, \"touch /etc/appsandbox-ssh-enabled\" ]\n"
            : "");

    fclose(f);
    SecureZeroMemory(hostname, sizeof(hostname));
    SecureZeroMemory(username, sizeof(username));
    return TRUE;
}

/* Write `network-config` — cloud-init NoCloud network config.

   For NAT-mode VMs (nat_ip set), emit a static netplan with the
   host-allocated address, gateway = <subnet>.1, and DNS so cloud-init
   brings eth0 up at first boot. Without this, the dracut-generated
   catch-all /run/systemd/network/zzzz-dracut-default.network DHCPs eth0,
   which wedges on NAT VMs (no DHCP server in HCN NAT).

   For non-NAT modes (nat_ip NULL), emit a link-only entry with optional:true
   so wait-online doesn't block — the agent or upstream DHCP handles the
   real address later.

   On subsequent boots the agent overrides this via /etc/netplan/99-appsandbox.yaml
   (higher-numbered yaml wins in netplan), so any host-side IP reassignment
   takes effect without needing to regenerate cidata. */
static BOOL generate_network_config(const wchar_t *path, const char *nat_ip)
{
    FILE *f;
    int a, b, c, d;

    if (!nat_ip || !nat_ip[0]) return FALSE;
    if (sscanf_s(nat_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return FALSE;
    /* "wb" — see generate_autoinstall_userdata comment about CRLF. */
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return FALSE;

    /* Gateway = <base>.1 per HCN NAT topology (see vm_agent.c:373). DNS =
       public resolvers only; the HCN NAT gateway doesn't run a DNS
       forwarder by default, so pointing at .1 makes every lookup wait
       through the resolver timeout. 1.1.1.1 and 8.8.8.8 are reachable
       through the NAT and respond promptly. */
    fprintf(f,
        "version: 2\n"
        "ethernets:\n"
        "  eth0:\n"
        "    dhcp4: false\n"
        "    dhcp6: false\n"
        "    accept-ra: false\n"
        "    addresses:\n"
        "      - %d.%d.%d.%d/24\n"
        "    routes:\n"
        "      - to: default\n"
        "        via: %d.%d.%d.1\n"
        "    nameservers:\n"
        "      addresses: [1.1.1.1, 8.8.8.8]\n",
        a, b, c, d,
        a, b, c);
    fclose(f);
    return TRUE;
}

/* Write `meta-data`. Tiny — just instance-id + local-hostname. cloud-init
   requires this file to exist on a NoCloud datasource even if empty. */
static BOOL generate_autoinstall_metadata(const wchar_t *path, const wchar_t *vm_name)
{
    FILE *f;
    char hostname[64];

    /* "wb" — see generate_autoinstall_userdata comment about CRLF. */
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return FALSE;

    WideCharToMultiByte(CP_UTF8, 0, vm_name, -1, hostname, sizeof(hostname), NULL, NULL);

    fprintf(f,
        "instance-id: appsandbox-%s\n"
        "local-hostname: %s\n",
        hostname, hostname);

    fclose(f);
    return TRUE;
}

/* Write setup.sh — runs in the BOOTED Ubuntu system, called from
   cloud-init's runcmd. By the time we get here:
     - networking is up
     - apt has run and ubuntu-desktop is installed
     - the cidata ISO is mounted at /mnt/cidata and the extras tree has
       been copied into /opt/appsandbox already
   So we just need to: install agent binaries, register systemd units,
   build/install kernel modules, extract wsl-mesa, copy wsl-deps libs.
   No chroot/curtin context — every path is a normal absolute path.

   This is a pretty long shell script. Keeping it inline (rather than
   shipping it as a separate file in the repo) keeps the user-data ↔ setup
   relationship close — they're both build artifacts of the same VM-create
   step. */
static BOOL generate_ubuntu_setup_sh(const wchar_t *path, BOOL ssh_enabled)
{
    FILE *f;
    (void)ssh_enabled;  /* ssh state carried via /etc/appsandbox-ssh-enabled marker */
    /* "wb" — CRITICAL for shell scripts. Without binary mode, the Windows
       CRT translates "\n" → "\r\n" and the kernel execve looks for an
       interpreter named "/bin/bash\r" (literal trailing CR), failing with
       ENOENT and a misleading "No such file or directory" message in
       systemd. Verified via SSH into a booted VM: head -1 setup.sh | od
       showed the trailing \r. */
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return FALSE;

    fputs(
        "#!/bin/bash\n"
        "# AppSandbox first-boot setup. Run by appsandbox-firstboot.service,\n"
        "# which is gated on:\n"
        "#   - appsandbox-agent.service (already up, vsock connection live)\n"
        "#   - network-online.target (fires AFTER the host has sent set_ip to\n"
        "#     the agent and netplan apply succeeded)\n"
        "#\n"
        "# So by the time we run, the static IP is on the wire and internet\n"
        "# is reachable. This script does the heavy network-dependent work\n"
        "# (apt install desktop, kernel module install, etc.) and then\n"
        "# reboots into the graphical target.\n"
        "set -u\n"
        "LOG=/var/log/appsandbox/setup.log\n"
        "mkdir -p /var/log/appsandbox\n"
        "exec >> \"$LOG\" 2>&1\n"
        "echo \"=== AppSandbox setup.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) ===\"\n"
        "EXTRAS=/opt/appsandbox\n"
        "cd \"$EXTRAS\" || { echo FATAL: no $EXTRAS ; exit 1; }\n"
        "\n"
        "# Verify connectivity defensively. firstboot.service depends on\n"
        "# network-online.target so this should already be true, but apt\n"
        "# silently fails in mysterious ways if DNS is broken. Poll for the\n"
        "# Ubuntu archive being reachable; bail loud if it isn't.\n"
        "echo \"Verifying DNS (max 60s)...\"\n"
        "for i in $(seq 1 30); do\n"
        "    if getent hosts archive.ubuntu.com >/dev/null 2>&1; then\n"
        "        echo \"DNS up after ${i}*2s.\"; break\n"
        "    fi\n"
        "    sleep 2\n"
        "done\n"
        "if ! getent hosts archive.ubuntu.com >/dev/null 2>&1; then\n"
        "    echo \"FATAL: DNS / archive.ubuntu.com not reachable. Check agent set_ip / netplan.\"\n"
        "    ip -4 addr show; ip -4 route show; cat /etc/resolv.conf 2>/dev/null\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "TGT_KVER=$(uname -r)\n"
        "echo \"Running kernel: $TGT_KVER\"\n"
        "\n"
        "# --- 1. apt up-front: NOTHING.\n"
        "# Desktop ISO autoinstall already installed ubuntu-desktop-minimal.\n"
        "# dkms / build-essential / headers / zstd / curl are only needed for\n"
        "# specific fallback paths below, so we install them on demand if\n"
        "# (and only if) they're actually triggered.\n"
        "\n"
        "# --- 2. SSH if requested (marker dropped by cloud-init bootcmd) ---\n"
        "if [ -f /etc/appsandbox-ssh-enabled ]; then\n"
        "    echo \"Installing openssh-server...\"\n"
        "    DEBIAN_FRONTEND=noninteractive apt-get install -y openssh-server\n"
        "    for unit in ssh.service ssh.socket; do\n"
        "        if systemctl list-unit-files 2>/dev/null | grep -q \"^${unit}\"; then\n"
        "            systemctl unmask \"$unit\" 2>/dev/null || true\n"
        "            systemctl enable --now \"$unit\" 2>/dev/null || true\n"
        "        fi\n"
        "    done\n"
        "    if command -v ufw >/dev/null 2>&1; then\n"
        "        ufw allow OpenSSH 2>/dev/null || ufw allow 22/tcp 2>/dev/null || true\n"
        "    fi\n"
        "fi\n"
        "\n"
        "# --- 3. Rest of the agent service family ---\n"
        "# (appsandbox-agent.service was already installed and started by\n"
        "#  cloud-init bootcmd. The others come up here, now that they have\n"
        "#  the libraries they need from the desktop install.)\n"
        "for bin in appsandbox-audio appsandbox-clipboard appsandbox-display appsandbox-input; do\n"
        "    if [ -f \"$EXTRAS/$bin\" ]; then\n"
        "        install -m 0755 \"$EXTRAS/$bin\" /usr/local/bin/\n"
        "    fi\n"
        "done\n"
        "for unit in appsandbox-audio.service appsandbox-display.service appsandbox-input.service; do\n"
        "    if [ -f \"$EXTRAS/systemd/$unit\" ]; then\n"
        "        install -m 0644 \"$EXTRAS/systemd/$unit\" /etc/systemd/system/\n"
        "    fi\n"
        "done\n"
        "# Clipboard unit is user-level (graphical-session.target).\n"
        "if [ -f \"$EXTRAS/systemd/appsandbox-clipboard.service\" ]; then\n"
        "    install -d /etc/systemd/user\n"
        "    install -m 0644 \"$EXTRAS/systemd/appsandbox-clipboard.service\" /etc/systemd/user/\n"
        "fi\n"
        "# modules-load.d configs (snd-aloop for audio, asb_drm for display)\n"
        "for conf in \"$EXTRAS\"/systemd/modules-load.d-*.conf; do\n"
        "    [ -f \"$conf\" ] || continue\n"
        "    dest=/etc/modules-load.d/$(basename \"$conf\" | sed 's/^modules-load.d-//')\n"
        "    install -m 0644 \"$conf\" \"$dest\"\n"
        "done\n"
        "[ -f \"$EXTRAS/modprobe.d-asb_drm.conf\" ] && \\\n"
        "    install -m 0644 \"$EXTRAS/modprobe.d-asb_drm.conf\" /etc/modprobe.d/asb_drm.conf\n"
        "\n"
        "# --- 4 + 5. Kernel modules — defensive rebuild.\n"
        "# Normally built during autoinstall.late-commands, but if late-\n"
        "# commands ran before subiquity's security-update kernel bump\n"
        "# (or there's been any kernel update since), DKMS may not have\n"
        "# built for the running kernel yet. Idempotent: dkms is a no-op\n"
        "# if .ko already exists for $TGT_KVER. Then load both. ---\n"
        "echo dxgkrnl > /etc/modules-load.d/dxgkrnl.conf\n"
        "if ! ls \"/lib/modules/$TGT_KVER/updates/dkms/\"asb_drm.ko* 2>/dev/null >/dev/null || \\\n"
        "   ! ls \"/lib/modules/$TGT_KVER/updates/dkms/\"dxgkrnl.ko* 2>/dev/null >/dev/null; then\n"
        "    echo \"DKMS modules missing for $TGT_KVER — building now...\"\n"
        "    dkms autoinstall -k \"$TGT_KVER\" 2>&1 || \\\n"
        "        echo \"WARN: dkms autoinstall failed; modprobe will fail\"\n"
        "fi\n"
        "modprobe asb_drm 2>&1 || echo \"WARN: modprobe asb_drm failed\"\n"
        "modprobe dxgkrnl 2>&1 || echo \"WARN: modprobe dxgkrnl failed\"\n"
        "\n"
        "# --- 6. wsl-mesa tarball -> /opt/wsl-mesa, ld.so.conf.d, Vulkan ICD ---\n"
        "if [ -f \"$EXTRAS/wsl-mesa.tar.zst\" ]; then\n"
        "    # zstd may not be in the Desktop ISO's package set; install on\n"
        "    # demand only when we actually need it.\n"
        "    if ! command -v zstd >/dev/null 2>&1; then\n"
        "        DEBIAN_FRONTEND=noninteractive apt-get install -y zstd\n"
        "    fi\n"
        "    zstd -d \"$EXTRAS/wsl-mesa.tar.zst\" -c | tar -C / -x\n"
        "    echo /opt/wsl-mesa/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/wsl-mesa.conf\n"
        "    install -d /etc/vulkan/icd.d\n"
        "    # microsoft-experimental Vulkan driver's ICD JSON.\n"
        "    if [ -f /opt/wsl-mesa/share/vulkan/icd.d/dzn_icd.x86_64.json ]; then\n"
        "        ln -sf /opt/wsl-mesa/share/vulkan/icd.d/dzn_icd.x86_64.json \\\n"
        "               /etc/vulkan/icd.d/dzn_icd.x86_64.json\n"
        "    fi\n"
        "fi\n"
        "\n"
        "# --- 7. wsl-deps (libd3d12, libd3d12core, libdxcore) on ldconfig path ---\n"
        "# These .so files are dlopen'd by Mesa's d3d12 gallium and dzn Vulkan\n"
        "# drivers when an app picks them via GALLIUM_DRIVER=d3d12 / VK_DRIVER_FILES.\n"
        "# They live at /opt/appsandbox/wsl-deps (NOT /usr/lib/wsl/lib — the\n"
        "# agent's 9P mount of host driver libs overlays that path).\n"
        "if [ -d /opt/appsandbox/wsl-deps ]; then\n"
        "    echo /opt/appsandbox/wsl-deps > /etc/ld.so.conf.d/appsandbox-wsl-deps.conf\n"
        "fi\n"
        "ldconfig\n"
        "\n"
        "# --- 8. daemon-reload, enable the helper services, set graphical ---\n"
        "# (Helper binaries + service files are already installed by section 3.\n"
        "#  Clipboard has no system service — agent spawns it as a child.)\n"
        "systemctl daemon-reload\n"
        "systemctl enable --now appsandbox-audio.service \\\n"
        "                       appsandbox-display.service \\\n"
        "                       appsandbox-input.service || true\n"
        "systemctl set-default graphical.target\n"
        "\n"
        "echo \"=== AppSandbox setup.sh finished $(date -u +%Y-%m-%dT%H:%M:%SZ), rebooting ===\"\n"
        "# Reboot so the desktop install + new units take effect cleanly.\n"
        "# firstboot.service's ExecStartPost will rm the marker after this\n"
        "# script exits 0, so the service won't re-run on the next boot.\n"
        "( sleep 5; systemctl reboot ) &\n",
        f);

    fclose(f);
    return TRUE;
}

/* ---- Resource staging ----
 *
 * Lay out a directory tree under `staging/extras/` that mirrors what
 * setup.sh expects. IMAPI2's AddTree() then turns the whole staging dir
 * into the ISO contents recursively, so we don't need ISO-level pack code
 * here — just file copies and directory creates.
 *
 * Source resolution: each artifact is searched in two places in order:
 *   1. `<res_dir>\linux\<file>` — packaged-with-installer location
 *   2. repo source tree path — convenient during development
 * If neither exists we log a warning and skip; the resulting VM just won't
 * have that capability (e.g. no dxgkrnl prebuilt -> DKMS path kicks in;
 * no DKMS source either -> GPU-PV is broken but everything else works).
 */

/* Find the repo root by walking up from the running .exe. Looks for the
   distinctive "tools" + "src" + "appsandbox.sln" trio so we don't false-match
   on someone's home directory. Returns TRUE if found. */
static BOOL find_repo_root(wchar_t *out, size_t out_len)
{
    wchar_t dir[MAX_PATH];
    wchar_t marker[MAX_PATH];
    int up;

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH)) return FALSE;
    {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = L'\0';
    }

    /* Walk up at most 8 levels */
    for (up = 0; up < 8; up++) {
        swprintf_s(marker, MAX_PATH, L"%s\\AppSandbox.sln", dir);
        if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(out, out_len, dir);
            return TRUE;
        }
        {
            wchar_t *slash = wcsrchr(dir, L'\\');
            if (!slash) return FALSE;
            *slash = L'\0';
        }
    }
    return FALSE;
}

/* Try to copy <linux_subdir>\<file> to dst from any of:
     1. res_dir + L"\\linux\\" + linux_subdir + L"\\" + file
     2. repo_root + L"\\" + repo_subdir + L"\\" + file
   Returns TRUE if copied. Both subdirs may be NULL. */
static BOOL copy_linux_artifact(const wchar_t *res_dir, const wchar_t *repo_root,
                                 const wchar_t *res_subpath,
                                 const wchar_t *repo_subpath,
                                 const wchar_t *dst)
{
    wchar_t src[MAX_PATH];

    if (res_dir && res_subpath) {
        swprintf_s(src, MAX_PATH, L"%s\\linux\\%s", res_dir, res_subpath);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            return CopyFileW(src, dst, FALSE);
    }
    if (repo_root && repo_subpath) {
        swprintf_s(src, MAX_PATH, L"%s\\%s", repo_root, repo_subpath);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            return CopyFileW(src, dst, FALSE);
    }
    return FALSE;
}

/* Recursive directory copy. Skips the .git directory if it appears. */
static int copy_dir_recursive(const wchar_t *src_dir, const wchar_t *dst_dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];
    int count = 0;

    if (!CreateDirectoryW(dst_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", src_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        if (_wcsicmp(fd.cFileName, L".git") == 0) continue;

        swprintf_s(src, MAX_PATH, L"%s\\%s", src_dir, fd.cFileName);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", dst_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += copy_dir_recursive(src, dst);
        } else {
            if (CopyFileW(src, dst, FALSE)) count++;
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return count;
}

/* Lay out everything that setup.sh expects under staging\extras\.
 *
 * Path resolution per artifact kind:
 *   - Build outputs (agent ELFs, .ko modules, wsl-mesa tarball)
 *       1. <res_dir>\linux\... — primary path. <res_dir> is bin\<cfg>\resources\
 *          at runtime, populated by AppSandbox.vcxproj's PostBuildEvent which
 *          xcopies the committed release\resources\ tree. So files committed
 *          under release\resources\linux\ flow here automatically.
 *       2. <repo>\tools\linux\dist\... — dev fallback. Set by `make` on the
 *          Linux dev box; useful when running AppSandbox.exe straight out of
 *          the build tree before doing the release/resources/linux/ copy.
 *
 *   - Source-only artifacts (systemd units, DKMS source trees, modprobe.d
 *     configs): read straight from <repo>\tools\linux\<sub>\... — these
 *     are stable text files in the repo, never built, no point routing them
 *     through dist/.
 *
 *   - wsl-deps .so libs: prefetched into C:\ProgramData\AppSandbox\wsl-deps\
 *     by `tools/wsl-deps/fetch-wsl-deps.ps1` (see tools/wsl-deps/README.md).
 */
static void stage_linux_agent_and_extras(const wchar_t *staging,
                                          const wchar_t *res_dir,
                                          BOOL ssh_enabled)
{
    wchar_t extras[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    wchar_t repo[MAX_PATH];
    BOOL have_repo;
    int i;

    (void)ssh_enabled;  /* SSH packaging is autoinstall-side; nothing to stage */

    have_repo = find_repo_root(repo, MAX_PATH);
    if (have_repo)
        ui_log(L"Staging Linux extras (repo root: %s)...", repo);
    else
        ui_log(L"Staging Linux extras (no repo root found; using res_dir only)...");

    swprintf_s(extras, MAX_PATH, L"%s\\extras", staging);
    CreateDirectoryW(extras, NULL);

    /* --- 1. Agent ELFs (5 of them; no separate clipboard-reader on Linux). --- */
    {
        const wchar_t *bins[] = {
            L"appsandbox-agent", L"appsandbox-audio", L"appsandbox-clipboard",
            L"appsandbox-display", L"appsandbox-input"
        };
        for (i = 0; i < (int)(sizeof(bins) / sizeof(bins[0])); i++) {
            wchar_t res_sub[MAX_PATH], repo_sub[MAX_PATH];
            swprintf_s(res_sub, MAX_PATH, L"%s", bins[i]);
            swprintf_s(repo_sub, MAX_PATH, L"tools\\linux\\dist\\%s", bins[i]);
            swprintf_s(dst, MAX_PATH, L"%s\\%s", extras, bins[i]);
            if (!copy_linux_artifact(res_dir, have_repo ? repo : NULL,
                                      res_sub, repo_sub, dst))
                ui_log(L"Warning: %s not built/staged (build tools/linux on a "
                       L"Linux box, copy dist/ into release\\resources\\linux\\)",
                       bins[i]);
        }
    }

    /* --- 2. Kernel modules: copy whole modules/<kver>/ tree as-is. --- */
    {
        wchar_t modules_dst[MAX_PATH];
        BOOL copied = FALSE;
        swprintf_s(modules_dst, MAX_PATH, L"%s\\modules", extras);

        /* Primary: <res_dir>\linux\modules\ */
        swprintf_s(src, MAX_PATH, L"%s\\linux\\modules", res_dir);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
            int n = copy_dir_recursive(src, modules_dst);
            if (n > 0) {
                ui_log(L"Staged %d kernel module file(s) from release/resources/linux/.", n);
                copied = TRUE;
            }
        }
        /* Dev fallback: <repo>\tools\linux\dist\modules\ */
        if (!copied && have_repo) {
            swprintf_s(src, MAX_PATH, L"%s\\tools\\linux\\dist\\modules", repo);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                int n = copy_dir_recursive(src, modules_dst);
                if (n > 0) {
                    ui_log(L"Staged %d kernel module file(s) from tools/linux/dist/.", n);
                    copied = TRUE;
                }
            }
        }
        if (!copied)
            ui_log(L"Warning: no prebuilt .ko modules staged "
                   L"(DKMS source-build fallback will run in guest at boot).");
    }

    /* --- 3. DKMS source trees (always from repo source). Each module's source
       gets staged under extras\<mod>-src\, matching the layout setup.sh
       expects. */
    if (have_repo) {
        swprintf_s(src, MAX_PATH, L"%s\\tools\\linux\\dxgkrnl\\src", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\dxgkrnl-src", extras);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            copy_dir_recursive(src, dst);

        swprintf_s(src, MAX_PATH, L"%s\\tools\\linux\\asb_drm", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\asb_drm-src", extras);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            copy_dir_recursive(src, dst);
    }

    /* --- 4. systemd units + modules-load.d + modprobe.d (always from repo). */
    if (have_repo) {
        wchar_t systemd_dst[MAX_PATH];
        swprintf_s(systemd_dst, MAX_PATH, L"%s\\systemd", extras);
        CreateDirectoryW(systemd_dst, NULL);

        /* Service unit files under tools\linux\agent\systemd\.
           appsandbox-firstboot.service is new — runs setup.sh once after
           the agent comes online and applies its host-assigned IP. */
        const wchar_t *units[] = {
            L"appsandbox-agent.service",
            L"appsandbox-audio.service",
            L"appsandbox-clipboard.service",
            L"appsandbox-display.service",
            L"appsandbox-input.service",
            L"appsandbox-firstboot.service"
        };
        for (i = 0; i < (int)(sizeof(units) / sizeof(units[0])); i++) {
            swprintf_s(src, MAX_PATH,
                L"%s\\tools\\linux\\agent\\systemd\\%s", repo, units[i]);
            swprintf_s(dst, MAX_PATH, L"%s\\%s", systemd_dst, units[i]);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(src, dst, FALSE);
        }

        /* modules-load.d configs live with their respective modules. */
        swprintf_s(src, MAX_PATH,
            L"%s\\tools\\linux\\agent\\modules-load.d-snd-aloop.conf", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\modules-load.d-snd-aloop.conf", systemd_dst);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(src, dst, FALSE);

        swprintf_s(src, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modules-load.d-asb_drm.conf", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\modules-load.d-asb_drm.conf", systemd_dst);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(src, dst, FALSE);

        /* asb-evict-simpledrm.service: unbinds the platform simple-framebuffer
           device before display-manager starts. `video=efifb:off video=simplefb:off`
           on the kernel cmdline doesn't kill simpledrm (built-in driver binding
           via sysfb), so mutter sees a phantom 1024x768 second monitor that
           captures the absolute pointer. */
        swprintf_s(src, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\systemd-asb-evict-simpledrm.service", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\asb-evict-simpledrm.service", systemd_dst);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(src, dst, FALSE);

        /* modprobe.d-asb_drm.conf goes at extras root (setup.sh expects it). */
        swprintf_s(src, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modprobe.d-asb_drm.conf", repo);
        swprintf_s(dst, MAX_PATH, L"%s\\modprobe.d-asb_drm.conf", extras);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(src, dst, FALSE);
    }

    /* --- 5. wsl-mesa tarball (build output). --- */
    {
        wchar_t res_sub[MAX_PATH], repo_sub[MAX_PATH];
        wcscpy_s(res_sub, MAX_PATH, L"wsl-mesa.tar.zst");
        wcscpy_s(repo_sub, MAX_PATH,
                 L"tools\\linux\\wsl-mesa\\prebuilt\\ubuntu-26.04-amd64\\wsl-mesa.tar.zst");
        swprintf_s(dst, MAX_PATH, L"%s\\wsl-mesa.tar.zst", extras);
        (void)copy_linux_artifact(res_dir, have_repo ? repo : NULL,
                                   res_sub, repo_sub, dst);
        /* Silent if missing — Mesa hardware-accel is optional; absence just
           means GPU-PV falls back to software rendering. */

        /* Stage the systemd-user-environment-generator + gnome-shell drop-in
           that gate Mesa-d3d12 onto every .desktop launch while keeping the
           compositor on llvmpipe. Both ship from tools/linux/wsl-mesa/. */
        if (have_repo) {
            swprintf_s(src, MAX_PATH,
                L"%s\\tools\\linux\\wsl-mesa\\50-appsandbox-gpu", repo);
            swprintf_s(dst, MAX_PATH, L"%s\\50-appsandbox-gpu", extras);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(src, dst, FALSE);

            swprintf_s(src, MAX_PATH,
                L"%s\\tools\\linux\\wsl-mesa\\org.gnome.Shell-no-gpu.conf", repo);
            swprintf_s(dst, MAX_PATH, L"%s\\org.gnome.Shell-no-gpu.conf", extras);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(src, dst, FALSE);

            swprintf_s(src, MAX_PATH,
                L"%s\\tools\\linux\\wsl-mesa\\appsandbox-gpu", repo);
            swprintf_s(dst, MAX_PATH, L"%s\\appsandbox-gpu", extras);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(src, dst, FALSE);
        }
    }

    /* --- 6. wsl-deps: 3 .so files prefetched into ProgramData. --- */
    {
        wchar_t wsldeps_dst[MAX_PATH];
        wchar_t programdata[MAX_PATH];
        const wchar_t *libs[] = { L"libd3d12.so", L"libd3d12core.so", L"libdxcore.so" };
        BOOL any = FALSE;

        swprintf_s(wsldeps_dst, MAX_PATH, L"%s\\wsl-deps", extras);
        CreateDirectoryW(wsldeps_dst, NULL);

        if (!GetEnvironmentVariableW(L"ProgramData", programdata, MAX_PATH))
            wcscpy_s(programdata, MAX_PATH, L"C:\\ProgramData");

        for (i = 0; i < 3; i++) {
            swprintf_s(src, MAX_PATH, L"%s\\AppSandbox\\wsl-deps\\current\\lib\\%s",
                       programdata, libs[i]);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                swprintf_s(dst, MAX_PATH, L"%s\\%s", wsldeps_dst, libs[i]);
                if (CopyFileW(src, dst, FALSE)) any = TRUE;
            }
        }
        if (!any)
            ui_log(L"Warning: wsl-deps libs not staged "
                   L"(run tools/wsl-deps/fetch-wsl-deps.ps1 to populate "
                   L"ProgramData\\AppSandbox\\wsl-deps\\)");
    }
}

HRESULT iso_create_resources_ubuntu(const wchar_t *iso_path,
                                     const wchar_t *vm_name,
                                     const wchar_t *admin_user,
                                     wchar_t *admin_pass,
                                     const wchar_t *res_dir,
                                     BOOL ssh_enabled,
                                     const char *nat_ip)
{
    wchar_t dir[MAX_PATH];
    wchar_t staging[MAX_PATH];
    wchar_t file_path[MAX_PATH];
    wchar_t *last_slash;
    char    pw_hash[128];
    HRESULT hr;
    BOOL    ok;

    if (!iso_path || !vm_name || !admin_user || !admin_pass)
        return E_INVALIDARG;

    /* Hash the password into glibc $6$ format for autoinstall identity.password.
       Wipe the plaintext immediately after — same pattern as the Windows path. */
    ok = unix_password_hash(admin_pass, pw_hash, sizeof(pw_hash));
    SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));
    if (!ok) {
        ui_log(L"Error: failed to hash Linux admin password.");
        return E_FAIL;
    }

    /* Derive output directory from iso_path. */
    wcscpy_s(dir, MAX_PATH, iso_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';

    /* Clean up any stale ISO and staging dir. */
    if (GetFileAttributesW(iso_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(iso_path);

    swprintf_s(staging, MAX_PATH, L"%s\\_iso_staging", dir);
    CreateDirectoryW(staging, NULL);

    ui_log(L"Creating Ubuntu cidata ISO...");

    /* 1. user-data (autoinstall YAML) at staging root */
    swprintf_s(file_path, MAX_PATH, L"%s\\user-data", staging);
    if (!generate_autoinstall_userdata(file_path, vm_name, admin_user, pw_hash, ssh_enabled)) {
        ui_log(L"Error: failed to write user-data");
        SecureZeroMemory(pw_hash, sizeof(pw_hash));
        return E_FAIL;
    }
    /* Also preserve a copy outside the staging dir so it survives the
       cleanup at end-of-create — lets us diff the generated YAML against
       cloud-init's schema without rebuilding/rebooting. */
    {
        wchar_t programdata[MAX_PATH], debug_dir[MAX_PATH], debug_path[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramData", programdata, MAX_PATH))
            wcscpy_s(programdata, MAX_PATH, L"C:\\ProgramData");
        swprintf_s(debug_dir, MAX_PATH, L"%s\\AppSandbox\\debug", programdata);
        CreateDirectoryW(debug_dir, NULL);
        swprintf_s(debug_path, MAX_PATH, L"%s\\last-user-data.yaml", debug_dir);
        CopyFileW(file_path, debug_path, FALSE);
        ui_log(L"Preserved user-data copy at %s", debug_path);
    }

    /* 2. meta-data at staging root */
    swprintf_s(file_path, MAX_PATH, L"%s\\meta-data", staging);
    if (!generate_autoinstall_metadata(file_path, vm_name))
        ui_log(L"Warning: failed to write meta-data");

    /* 3. network-config at staging root — NAT mode only. For NAT, write a
       static netplan with the host-allocated address so cloud-init brings
       eth0 up at first boot (HCN NAT has no DHCP server, agent assigns
       later via 99-appsandbox.yaml). For external mode we skip this file
       entirely; cloud-init's NoCloud default (DHCP on eth*) then kicks in
       and the upstream DHCP server gives us a lease. */
    if (nat_ip && nat_ip[0]) {
        swprintf_s(file_path, MAX_PATH, L"%s\\network-config", staging);
        if (!generate_network_config(file_path, nat_ip))
            ui_log(L"Warning: failed to write network-config");
    }

    /* 4. extras/ tree (agent ELFs, dxgkrnl, asb_drm, wsl-mesa, wsl-deps, systemd) */
    stage_linux_agent_and_extras(staging, res_dir, ssh_enabled);

    /* 4. extras/setup.sh — runs inside target chroot from autoinstall
       late-commands, wires up everything stage_linux_agent_and_extras
       just laid out. */
    {
        wchar_t extras[MAX_PATH];
        swprintf_s(extras, MAX_PATH, L"%s\\extras", staging);
        CreateDirectoryW(extras, NULL);
        swprintf_s(file_path, MAX_PATH, L"%s\\setup.sh", extras);
        if (!generate_ubuntu_setup_sh(file_path, ssh_enabled))
            ui_log(L"Warning: failed to write setup.sh");
    }

    /* 5. Build the ISO. KEY: volume label MUST be exactly "cidata" (lowercase)
       so cloud-init's NoCloud datasource discovery finds it via `blkid -L cidata`. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = create_iso_from_dir(iso_path, staging, L"cidata");
    CoUninitialize();

    /* Clean up. */
    SecureZeroMemory(pw_hash, sizeof(pw_hash));
    remove_staging_dir(staging);

    if (SUCCEEDED(hr))
        ui_log(L"Ubuntu cidata ISO created: %s", iso_path);
    else
        ui_log(L"Error: failed to build cidata ISO (0x%08X)", hr);

    return hr;
}

/* ============================================================================
 *  Ubuntu cloud image download + VHD-footer wrap + VHDX conversion
 * ============================================================================
 *
 *  Pipeline: cloud-images.ubuntu.com publishes Ubuntu 26.04 as a .tar.gz
 *  containing a single raw disk image (`.img`, ~2.2 GB). That raw image
 *  is a complete bootable GPT-partitioned disk — ESP + ext4 root with
 *  the pre-installed Ubuntu Server + cloud-init. Same disk content that
 *  Azure runs on Hyper-V Gen 2 every day.
 *
 *  We just need to wrap the raw bytes in a Hyper-V-compatible container.
 *  Two steps:
 *
 *    1. Append a 512-byte VHD-fixed footer (so the file becomes a valid
 *       fixed-VHD). Format defined in Microsoft's "Virtual Hard Disk
 *       Image Format Specification", widely documented.
 *
 *    2. Call CreateVirtualDisk with SourcePath = our VHD, target type
 *       VHDX. Microsoft's own format driver copies the disk content
 *       into a fresh dynamic VHDX. Hyper-V Gen 2 boots VHDX, not VHD,
 *       so this conversion is the bit that makes the cached image
 *       usable as a parent for differencing children.
 *
 *  Cache layout:
 *     C:\ProgramData\AppSandbox\linux-base\ubuntu-26.04\base.vhdx
 *
 *  Idempotent: presence of base.vhdx means "already cached, return path."
 *  Marked read-only after creation so differencing children don't
 *  accidentally write through to the parent.
 * ========================================================================== */

/* Ubuntu 26.04 LTS cloud image — published as qcow2 (full GPT disk with
   ESP + ext4 rootfs + Ubuntu's signed shim/grub already installed). We
   read the qcow2 ourselves (qcow2_to_raw below), convert raw -> VHD ->
   VHDX, and hand the resulting VHDX to Hyper-V. */
#define UBUNTU_26_04_QCOW2_URL \
    L"https://cloud-images.ubuntu.com/releases/resolute/release/" \
    L"ubuntu-26.04-server-cloudimg-amd64.img"

/* VHD spec values for the footer's fixed fields. All BIG-ENDIAN on disk —
   the VHD format predates little-endian intel-only thinking. */
#define VHD_COOKIE          "conectix"
#define VHD_FEATURES_RESV   0x00000002u  /* Reserved-must-be-set bit */
#define VHD_FORMAT_VERSION  0x00010000u  /* Major=1 Minor=0 */
#define VHD_DATA_OFFSET_FIX 0xFFFFFFFFFFFFFFFFull  /* No dynamic header */
#define VHD_CREATOR_APP     "asb!"
#define VHD_CREATOR_VER     0x00010000u
#define VHD_HOST_OS_WIN     0x5769326Bu  /* "Wi2k" */
#define VHD_DISK_TYPE_FIXED 2u

/* Big-endian writers — VHD footer fields are all BE regardless of CPU. */
static void put_be32(BYTE *p, DWORD v) {
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16);
    p[2] = (BYTE)(v >>  8); p[3] = (BYTE)v;
}
static void put_be64(BYTE *p, ULONGLONG v) {
    p[0] = (BYTE)(v >> 56); p[1] = (BYTE)(v >> 48);
    p[2] = (BYTE)(v >> 40); p[3] = (BYTE)(v >> 32);
    p[4] = (BYTE)(v >> 24); p[5] = (BYTE)(v >> 16);
    p[6] = (BYTE)(v >>  8); p[7] = (BYTE)v;
}

/* CHS geometry computation, lifted verbatim from the VHD spec appendix.
   For modern disks > ~8 GB, this always returns (65535, 16, 255). */
static void vhd_compute_chs(ULONGLONG total_sectors,
                             WORD *out_cyls, BYTE *out_heads, BYTE *out_spt)
{
    BYTE  heads, spt;
    DWORD cyl_times_heads;
    WORD  cylinders;

    if (total_sectors > 65535ULL * 16 * 255)
        total_sectors = 65535ULL * 16 * 255;

    if (total_sectors >= 65535ULL * 16 * 63) {
        spt = 255;
        heads = 16;
        cyl_times_heads = (DWORD)(total_sectors / spt);
    } else {
        spt = 17;
        cyl_times_heads = (DWORD)(total_sectors / spt);
        heads = (BYTE)((cyl_times_heads + 1023) / 1024);
        if (heads < 4) heads = 4;
        if (cyl_times_heads >= ((DWORD)heads * 1024) || heads > 16) {
            spt = 31;
            heads = 16;
            cyl_times_heads = (DWORD)(total_sectors / spt);
        }
        if (cyl_times_heads >= ((DWORD)heads * 1024)) {
            spt = 63;
            heads = 16;
            cyl_times_heads = (DWORD)(total_sectors / spt);
        }
    }
    cylinders = (WORD)(cyl_times_heads / heads);

    *out_cyls = cylinders;
    *out_heads = heads;
    *out_spt = spt;
}

/* ============================================================================
 *  qcow2 -> raw disk image
 * ============================================================================
 *
 *  qcow2 is QEMU's native image format and Ubuntu's standard cloud-image
 *  publishing format. We need raw bytes (or VHD/VHDX) for Hyper-V, so we
 *  walk the qcow2 cluster tables and write out the disk content.
 *
 *  Format reference: https://github.com/qemu/qemu/blob/master/docs/interop/qcow2.txt
 *
 *  Layout we support:
 *    - Version 2 or 3
 *    - cluster_bits 9..21 (we tested 16 = 64 KB; most cloud images use this)
 *    - crypt_method == 0  (no encryption)
 *    - incompatible_features == 0
 *    - No compressed clusters (modern Ubuntu cloud images don't compress;
 *      we error out cleanly if we see the compressed bit)
 *    - No backing file (cloud images are self-contained)
 *
 *  How addressing works:
 *    Disk is divided into clusters of cluster_size bytes (typically 64 KB).
 *    A two-level lookup translates a virtual cluster index to a physical
 *    file offset:
 *
 *      cluster_idx        = virtual_offset / cluster_size
 *      l2_entries         = cluster_size / 8
 *      l1_idx             = cluster_idx / l2_entries
 *      l2_idx             = cluster_idx % l2_entries
 *
 *      l2_table_offset    = l1_table[l1_idx] & ~RESERVED_BITS
 *      physical_offset    = l2_table[l2_idx] & ~RESERVED_BITS
 *
 *    L1 entry of 0 -> entire L2 range is unallocated (read as zeros).
 *    L2 entry of 0 -> that cluster is unallocated (read as zeros).
 *    Compressed bit (bit 62 of L2) -> we don't support, return error.
 */

#define QCOW2_MAGIC  0x514649FBu  /* "QFI\xfb" big-endian as a 32-bit number */
#define QCOW2_L1E_OFFSET_MASK    0x00FFFFFFFFFFFE00ULL  /* bits 9..55 of L1 entry */
#define QCOW2_L2E_COMPRESSED_BIT 0x4000000000000000ULL  /* bit 62 */
#define QCOW2_L2E_OFFSET_MASK    0x00FFFFFFFFFFFE00ULL  /* bits 9..55 (uncompressed) */
/* Compressed L2 entry uses bits 0..(x-1) for byte-offset and bits x..61
   for compressed-sectors-minus-1, where x = 62 - (cluster_bits - 8).
   For cluster_bits=16 -> x=54. Masks are computed at runtime since
   they depend on cluster_bits. */

/* ---- DEFLATE inflater (RFC 1951) + zlib stream wrapper (RFC 1950) ----
 *
 * Just enough to decompress qcow2's per-cluster zlib streams. No
 * external dependencies — written from the spec. Output buffer is
 * caller-supplied and bounded (one qcow2 cluster, typically 64 KB),
 * so we don't need a sliding window beyond the output itself.
 *
 * Errors return -1; success returns the number of output bytes
 * produced. Doesn't validate the trailing Adler-32 checksum since
 * qcow2 spec explicitly says "decompression stops when it has
 * produced a cluster of data" — i.e. the checksum is typically
 * beyond our truncation point.
 */

/* Length codes (symbols 257..285): base length and extra-bit count. */
static const WORD kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const BYTE kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Distance codes (symbols 0..29): base and extra-bit count. */
static const WORD kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const BYTE kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Order in which code-length-alphabet code lengths appear in the
   dynamic Huffman header (RFC 1951 section 3.2.7). */
static const BYTE kCLOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

typedef struct {
    SHORT count[16];     /* count[k] = number of symbols with code length k */
    SHORT symbol[288];   /* symbols ordered by length then symbol value */
} HuffTable;

typedef struct {
    const BYTE *src; size_t src_len; size_t src_pos;
    BYTE       *dst; size_t dst_cap; size_t dst_pos;
    DWORD       bit_buf;
    int         bit_count;
    int         error;   /* 1 if any helper hit EOF / malformed input */
} InflateState;

/* Read n bits (1..24) LSB-first. Returns the value, or -1 on EOF. */
static int infl_bits(InflateState *s, int n)
{
    int v;
    while (s->bit_count < n) {
        if (s->src_pos >= s->src_len) { s->error = 1; return -1; }
        s->bit_buf |= ((DWORD)s->src[s->src_pos++]) << s->bit_count;
        s->bit_count += 8;
    }
    v = (int)(s->bit_buf & (((DWORD)1 << n) - 1));
    s->bit_buf >>= n;
    s->bit_count -= n;
    return v;
}

/* Build a canonical Huffman decode table from `n` code-length entries.
   Symbols with length 0 are unused. Returns 0 on success, -1 on malformed
   input (e.g. over-subscribed code). */
static int infl_build(HuffTable *t, const BYTE *lens, int n)
{
    int left = 1;  /* +1 for the implicit "all bits unset" path */
    int i;
    SHORT offs[16];

    for (i = 0; i < 16; i++) t->count[i] = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] >= 16) return -1;
        t->count[lens[i]]++;
    }
    if (t->count[0] == n) return 0;  /* empty (no codes) */

    /* Detect malformed code lengths (too many or too few codes). */
    for (i = 1; i <= 15; i++) {
        left <<= 1;
        left -= t->count[i];
        if (left < 0) return -1;
    }
    /* `left > 0` means incomplete code; we accept it for the special case
       of a single-symbol code (count[1] == 1) which RFC 1951 allows. */
    if (left > 0 && !(left == 1 && t->count[1] == 1 && n > 1)) {
        /* Strictly malformed, but accept anyway — some encoders produce
           incomplete tables that aren't actually exercised. We'll catch
           bad lookups as decode errors. */
    }

    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i+1] = offs[i] + t->count[i];
    for (i = 0; i < n; i++) {
        if (lens[i] != 0) {
            t->symbol[offs[lens[i]]++] = (SHORT)i;
        }
    }
    return 0;
}

/* Decode one symbol using `t`. Returns the symbol, or -1 on error. */
static int infl_decode(InflateState *s, const HuffTable *t)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int bit = infl_bits(s, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (code - t->count[len] < first) {
            return t->symbol[index + (code - first)];
        }
        index += t->count[len];
        first = (first + t->count[len]) << 1;
    }
    s->error = 1;
    return -1;
}

/* Decode literal/length/distance codes for one block, using the tables
   already prepared in `litlen` and `dist`. Output goes into s->dst. */
static int infl_codes(InflateState *s, const HuffTable *litlen, const HuffTable *dist)
{
    int sym;
    while ((sym = infl_decode(s, litlen)) >= 0) {
        if (sym < 256) {
            if (s->dst_pos >= s->dst_cap) { s->error = 1; return -1; }
            s->dst[s->dst_pos++] = (BYTE)sym;
        } else if (sym == 256) {
            return 0;  /* end-of-block */
        } else {
            int idx = sym - 257;
            int extra, length, dist_sym, dist_extra, distance;
            if (idx < 0 || idx >= 29) { s->error = 1; return -1; }
            extra = infl_bits(s, kLenExtra[idx]);
            if (extra < 0) return -1;
            length = kLenBase[idx] + extra;
            dist_sym = infl_decode(s, dist);
            if (dist_sym < 0 || dist_sym >= 30) { s->error = 1; return -1; }
            dist_extra = infl_bits(s, kDistExtra[dist_sym]);
            if (dist_extra < 0) return -1;
            distance = kDistBase[dist_sym] + dist_extra;
            if ((size_t)distance > s->dst_pos) { s->error = 1; return -1; }
            /* Copy `length` bytes from `distance` bytes back. Sliding-window
               aliasing means we can't use memcpy; copy byte-by-byte so a
               distance < length still produces the correct run-length result. */
            {
                size_t base = s->dst_pos - distance;
                int i;
                if (s->dst_pos + length > s->dst_cap) { s->error = 1; return -1; }
                for (i = 0; i < length; i++) {
                    s->dst[s->dst_pos] = s->dst[base + i];
                    s->dst_pos++;
                }
            }
        }
    }
    return -1;
}

/* BTYPE=00: stored block (no compression). */
static int infl_stored(InflateState *s)
{
    int len, nlen;
    /* Discard remaining bits in bit_buf to align to a byte boundary. */
    s->bit_buf = 0;
    s->bit_count = 0;
    if (s->src_pos + 4 > s->src_len) { s->error = 1; return -1; }
    len  =  s->src[s->src_pos++];
    len |= ((int)s->src[s->src_pos++]) << 8;
    nlen =  s->src[s->src_pos++];
    nlen|= ((int)s->src[s->src_pos++]) << 8;
    if ((len ^ nlen) != 0xFFFF) { s->error = 1; return -1; }
    if (s->src_pos + len > s->src_len) { s->error = 1; return -1; }
    if (s->dst_pos + (size_t)len > s->dst_cap) { s->error = 1; return -1; }
    if (len > 0) {
        memcpy(s->dst + s->dst_pos, s->src + s->src_pos, len);
        s->src_pos += len;
        s->dst_pos += len;
    }
    return 0;
}

/* BTYPE=01: fixed Huffman codes. */
static int infl_fixed(InflateState *s)
{
    static HuffTable litlen, dist;
    static int built = 0;
    if (!built) {
        BYTE lens[288];
        int i;
        for (i =   0; i < 144; i++) lens[i] = 8;
        for (i = 144; i < 256; i++) lens[i] = 9;
        for (i = 256; i < 280; i++) lens[i] = 7;
        for (i = 280; i < 288; i++) lens[i] = 8;
        if (infl_build(&litlen, lens, 288) != 0) return -1;
        for (i =   0; i <  30; i++) lens[i] = 5;
        if (infl_build(&dist,   lens,  30) != 0) return -1;
        built = 1;
    }
    return infl_codes(s, &litlen, &dist);
}

/* BTYPE=10: dynamic Huffman codes. Reads code lengths from the stream,
   builds two Huffman tables (literal/length + distance), then decodes
   the block. */
static int infl_dynamic(InflateState *s)
{
    int hlit, hdist, hclen;
    BYTE cl_lens[19];
    BYTE all_lens[288 + 30];
    HuffTable cl_table, litlen, dist;
    int i, total;

    hlit  = infl_bits(s, 5); if (hlit  < 0) return -1; hlit  += 257;
    hdist = infl_bits(s, 5); if (hdist < 0) return -1; hdist += 1;
    hclen = infl_bits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30 || hclen > 19) { s->error = 1; return -1; }

    /* Read code-length-alphabet code lengths in the predefined order. */
    for (i = 0; i < 19; i++) cl_lens[i] = 0;
    for (i = 0; i < hclen; i++) {
        int v = infl_bits(s, 3); if (v < 0) return -1;
        cl_lens[kCLOrder[i]] = (BYTE)v;
    }
    if (infl_build(&cl_table, cl_lens, 19) != 0) { s->error = 1; return -1; }

    /* Use cl_table to read code lengths for litlen + dist (concatenated). */
    total = hlit + hdist;
    i = 0;
    while (i < total) {
        int sym = infl_decode(s, &cl_table);
        if (sym < 0) return -1;
        if (sym < 16) {
            all_lens[i++] = (BYTE)sym;
        } else if (sym == 16) {
            int rep = infl_bits(s, 2); if (rep < 0) return -1;
            rep += 3;
            if (i == 0) { s->error = 1; return -1; }
            while (rep-- && i < total) all_lens[i] = all_lens[i-1], i++;
        } else if (sym == 17) {
            int rep = infl_bits(s, 3); if (rep < 0) return -1;
            rep += 3;
            while (rep-- && i < total) all_lens[i++] = 0;
        } else if (sym == 18) {
            int rep = infl_bits(s, 7); if (rep < 0) return -1;
            rep += 11;
            while (rep-- && i < total) all_lens[i++] = 0;
        } else { s->error = 1; return -1; }
    }

    if (infl_build(&litlen, all_lens, hlit) != 0) { s->error = 1; return -1; }
    if (infl_build(&dist, all_lens + hlit, hdist) != 0) { s->error = 1; return -1; }
    return infl_codes(s, &litlen, &dist);
}

/* Top-level DEFLATE inflater (RFC 1951). */
static int inflate_deflate(BYTE *out, size_t out_max,
                            const BYTE *in, size_t in_len,
                            size_t *out_actual)
{
    InflateState s;
    int bfinal, btype;
    s.src = in; s.src_len = in_len; s.src_pos = 0;
    s.dst = out; s.dst_cap = out_max; s.dst_pos = 0;
    s.bit_buf = 0; s.bit_count = 0; s.error = 0;

    do {
        bfinal = infl_bits(&s, 1); if (bfinal < 0) return -1;
        btype  = infl_bits(&s, 2); if (btype  < 0) return -1;
        if (btype == 0) {
            if (infl_stored(&s) != 0) return -1;
        } else if (btype == 1) {
            if (infl_fixed(&s) != 0) return -1;
        } else if (btype == 2) {
            if (infl_dynamic(&s) != 0) return -1;
        } else {
            return -1;  /* reserved */
        }
        if (s.error) return -1;
    } while (!bfinal);

    if (out_actual) *out_actual = s.dst_pos;
    return 0;
}

/* zlib stream wrapper (RFC 1950): skip 2-byte CMF/FLG header, run the
   DEFLATE inflater, ignore trailing Adler-32 (qcow2 truncates streams
   at one-cluster boundaries anyway). */
static int inflate_zlib(BYTE *out, size_t out_max,
                         const BYTE *in, size_t in_len,
                         size_t *out_actual)
{
    if (in_len < 2) return -1;
    /* CMF byte: low nibble = 8 for DEFLATE; high nibble = log2(window)-8.
       FLG byte: bit 5 = FDICT (we don't handle preset dictionaries; refuse).
       (CMF*256 + FLG) must be a multiple of 31. */
    if ((in[0] & 0x0F) != 8) return -1;
    if ((in[1] & 0x20) != 0) return -1;  /* FDICT set — unsupported */
    if (((((int)in[0]) << 8) | in[1]) % 31 != 0) return -1;
    return inflate_deflate(out, out_max, in + 2, in_len - 2, out_actual);
}

/* Big-endian readers — qcow2 stores everything as big-endian on disk. */
static DWORD     read_be32(const BYTE *p) {
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) |
           ((DWORD)p[2] <<  8) |  (DWORD)p[3];
}
static ULONGLONG read_be64(const BYTE *p) {
    return ((ULONGLONG)read_be32(p) << 32) | (ULONGLONG)read_be32(p + 4);
}

/* Read exactly `len` bytes at `offset` into `buf`. Returns TRUE on success. */
static BOOL pread_full(HANDLE h, ULONGLONG offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    BYTE *p = (BYTE *)buf;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return FALSE;
    while (len > 0) {
        if (!ReadFile(h, p, len, &got, NULL) || got == 0)
            return FALSE;
        p += got; len -= got;
    }
    return TRUE;
}

/* Write `len` bytes at `offset` to `h`. NTFS makes unwritten regions
   sparse automatically if the file was created with FILE_ATTRIBUTE_SPARSE_FILE
   (and the FSCTL_SET_SPARSE ioctl), so we don't need to write zeros for
   unallocated qcow2 clusters — just skip them. */
static BOOL pwrite_full(HANDLE h, ULONGLONG offset, const void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD wrote = 0;
    const BYTE *p = (const BYTE *)buf;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return FALSE;
    while (len > 0) {
        if (!WriteFile(h, p, len, &wrote, NULL) || wrote == 0)
            return FALSE;
        p += wrote; len -= wrote;
    }
    return TRUE;
}

/* Convert a qcow2 file at `src_path` into a raw disk image at `dst_path`.
   The output file's size is exactly the qcow2 header's `virtual_size`,
   with all uncompressed clusters copied verbatim and unallocated clusters
   left as sparse zeros (NTFS sparse file). Returns S_OK on success. */
static HRESULT qcow2_to_raw(const wchar_t *src_path, const wchar_t *dst_path)
{
    HANDLE  hSrc = INVALID_HANDLE_VALUE;
    HANDLE  hDst = INVALID_HANDLE_VALUE;
    BYTE    header[112];
    BYTE   *l1_table = NULL;
    BYTE   *l2_table = NULL;
    BYTE   *cluster_buf = NULL;
    HRESULT hr = E_FAIL;
    DWORD   version, cluster_bits, l1_size, crypt_method;
    ULONGLONG virtual_size, l1_table_offset, incompat_features;
    DWORD   cluster_size, l2_entries;
    ULONGLONG total_clusters;
    DWORD   l2_table_bytes;
    ULONGLONG cluster_idx;

    hSrc = CreateFileW(src_path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE) {
        ui_log(L"qcow2_to_raw: open src failed (%lu)", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    /* --- Parse header (first 112 bytes covers up through v3 fields) --- */
    if (!pread_full(hSrc, 0, header, sizeof(header))) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    if (read_be32(header + 0) != QCOW2_MAGIC) {
        ui_log(L"qcow2_to_raw: bad magic (not a qcow2 file)");
        goto cleanup;
    }
    version           = read_be32(header + 4);
    cluster_bits      = read_be32(header + 20);
    virtual_size      = read_be64(header + 24);
    crypt_method      = read_be32(header + 32);
    l1_size           = read_be32(header + 36);
    l1_table_offset   = read_be64(header + 40);
    incompat_features = (version >= 3) ? read_be64(header + 72) : 0;

    if (version != 2 && version != 3) {
        ui_log(L"qcow2_to_raw: unsupported version %lu", version);
        goto cleanup;
    }
    if (crypt_method != 0) {
        ui_log(L"qcow2_to_raw: encrypted qcow2 not supported");
        goto cleanup;
    }
    if (incompat_features != 0) {
        ui_log(L"qcow2_to_raw: incompat_features=0x%llx not supported", incompat_features);
        goto cleanup;
    }
    if (cluster_bits < 9 || cluster_bits > 21) {
        ui_log(L"qcow2_to_raw: cluster_bits %lu out of range", cluster_bits);
        goto cleanup;
    }
    cluster_size = 1u << cluster_bits;
    l2_entries   = cluster_size / 8;
    total_clusters = (virtual_size + cluster_size - 1) / cluster_size;

    ui_log(L"qcow2_to_raw: v%lu, cluster=%lu B, virtual=%llu B (%llu clusters)",
           version, cluster_size, virtual_size, total_clusters);

    /* --- Read L1 table --- */
    {
        DWORD l1_bytes = l1_size * 8;
        l1_table = (BYTE *)HeapAlloc(GetProcessHeap(), 0, l1_bytes);
        if (!l1_table) { hr = E_OUTOFMEMORY; goto cleanup; }
        if (!pread_full(hSrc, l1_table_offset, l1_table, l1_bytes)) {
            ui_log(L"qcow2_to_raw: read L1 table failed (%lu)", GetLastError());
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        }
    }

    /* --- Create output, make sparse, set size --- */
    hDst = CreateFileW(dst_path, GENERIC_WRITE, 0,
                        NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDst == INVALID_HANDLE_VALUE) {
        ui_log(L"qcow2_to_raw: create dst failed (%lu)", GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    {
        DWORD bytes;
        DeviceIoControl(hDst, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytes, NULL);
    }
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)virtual_size;
        if (!SetFilePointerEx(hDst, li, NULL, FILE_BEGIN) || !SetEndOfFile(hDst)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        }
    }

    /* --- Allocate working buffers --- */
    l2_table_bytes = l2_entries * 8;
    l2_table = (BYTE *)HeapAlloc(GetProcessHeap(), 0, l2_table_bytes);
    cluster_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cluster_size);
    if (!l2_table || !cluster_buf) {
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    /* --- Walk every cluster. Cache one L2 table at a time (sequential
       reading means L2 reuse is high). --- */
    {
        ULONGLONG cached_l2_index = (ULONGLONG)-1;
        ULONG     last_pct = 0;

        for (cluster_idx = 0; cluster_idx < total_clusters; cluster_idx++) {
            ULONGLONG l1_idx = cluster_idx / l2_entries;
            ULONGLONG l2_idx = cluster_idx % l2_entries;
            ULONGLONG l1_entry, l2_table_off, l2_entry, cluster_off;

            if (l1_idx >= l1_size) break;  /* past end of L1 */

            l1_entry = read_be64(l1_table + l1_idx * 8);
            l2_table_off = l1_entry & QCOW2_L1E_OFFSET_MASK;

            if (l2_table_off == 0) {
                /* Entire L2 range unallocated. Skip the rest of this L2's
                   clusters in one jump rather than checking each. */
                cluster_idx = (l1_idx + 1) * l2_entries - 1;
                continue;
            }

            /* Cache L2 table on first hit; skip re-read while still on it. */
            if (l1_idx != cached_l2_index) {
                if (!pread_full(hSrc, l2_table_off, l2_table, l2_table_bytes)) {
                    ui_log(L"qcow2_to_raw: read L2 table failed (%lu)", GetLastError());
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    goto cleanup;
                }
                cached_l2_index = l1_idx;
            }

            l2_entry = read_be64(l2_table + l2_idx * 8);
            if (l2_entry == 0) continue;  /* unallocated cluster -> sparse zero */
            if (l2_entry & QCOW2_L2E_COMPRESSED_BIT) {
                /* Compressed cluster. Bit layout (qcow2 spec):
                     x = 62 - (cluster_bits - 8)
                     bits 0..x-1   : host_offset (in bytes, may be unaligned)
                     bits x..61    : compressed_sectors_minus_1 (8-bit for cb=16)
                   The data is a zlib stream that decompresses to one
                   cluster (cluster_size bytes). */
                DWORD x = 62 - (cluster_bits - 8);
                ULONGLONG comp_off    = l2_entry & (((ULONGLONG)1 << x) - 1);
                DWORD     comp_secs_m1= (DWORD)((l2_entry >> x) & (((ULONGLONG)1 << (62 - x)) - 1));
                DWORD     comp_bytes  = (comp_secs_m1 + 1) * 512u - (DWORD)(comp_off & 511);
                BYTE     *comp_buf;
                size_t    out_actual = 0;

                comp_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, comp_bytes);
                if (!comp_buf) { hr = E_OUTOFMEMORY; goto cleanup; }
                if (!pread_full(hSrc, comp_off, comp_buf, comp_bytes)) {
                    DWORD err = GetLastError();
                    HeapFree(GetProcessHeap(), 0, comp_buf);
                    ui_log(L"qcow2_to_raw: read compressed cluster idx %llu failed (%lu)",
                           cluster_idx, err);
                    hr = HRESULT_FROM_WIN32(err);
                    goto cleanup;
                }
                if (inflate_zlib(cluster_buf, cluster_size,
                                  comp_buf, comp_bytes, &out_actual) != 0) {
                    HeapFree(GetProcessHeap(), 0, comp_buf);
                    ui_log(L"qcow2_to_raw: inflate failed on cluster idx %llu", cluster_idx);
                    goto cleanup;
                }
                HeapFree(GetProcessHeap(), 0, comp_buf);
                if (out_actual < cluster_size) {
                    /* zlib stream ended early; zero-fill the remainder. */
                    memset(cluster_buf + out_actual, 0, cluster_size - out_actual);
                }
                if (!pwrite_full(hDst, cluster_idx * cluster_size, cluster_buf, cluster_size)) {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    goto cleanup;
                }
                continue;
            }
            cluster_off = l2_entry & QCOW2_L2E_OFFSET_MASK;
            if (cluster_off == 0) continue;

            if (!pread_full(hSrc, cluster_off, cluster_buf, cluster_size)) {
                ui_log(L"qcow2_to_raw: read cluster idx %llu failed (%lu)",
                       cluster_idx, GetLastError());
                hr = HRESULT_FROM_WIN32(GetLastError());
                goto cleanup;
            }
            if (!pwrite_full(hDst, cluster_idx * cluster_size, cluster_buf, cluster_size)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
                goto cleanup;
            }

            /* Progress log every 5%. */
            {
                ULONG pct = (ULONG)((cluster_idx + 1) * 100 / total_clusters);
                if (pct >= last_pct + 5) {
                    ui_log(L"qcow2_to_raw: %lu%% (%llu/%llu clusters)",
                           pct, cluster_idx + 1, total_clusters);
                    last_pct = pct;
                }
            }
        }
    }

    hr = S_OK;
    ui_log(L"qcow2_to_raw: done, output is %llu bytes", virtual_size);

cleanup:
    if (l1_table)    HeapFree(GetProcessHeap(), 0, l1_table);
    if (l2_table)    HeapFree(GetProcessHeap(), 0, l2_table);
    if (cluster_buf) HeapFree(GetProcessHeap(), 0, cluster_buf);
    if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
    if (hDst != INVALID_HANDLE_VALUE) CloseHandle(hDst);
    return hr;
}

/* Append a VHD-fixed footer to an existing raw disk file. After this
   call, `path` is a valid Microsoft VHD that Hyper-V/VirtDisk can open. */
static HRESULT vhd_append_fixed_footer(const wchar_t *path, ULONGLONG size_bytes)
{
    BYTE footer[512];
    HANDLE h;
    DWORD bytes_written;
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER vhd_epoch_ft;  /* 2000-01-01 00:00:00 UTC as FILETIME */
    DWORD timestamp;
    ULONGLONG total_sectors;
    WORD cyls; BYTE heads, spt;
    GUID unique_id;
    DWORD sum;
    int i;

    ZeroMemory(footer, sizeof(footer));

    /* Cookie */
    memcpy(footer + 0, VHD_COOKIE, 8);

    /* Features (BE) */
    put_be32(footer + 8, VHD_FEATURES_RESV);

    /* File Format Version (BE) */
    put_be32(footer + 12, VHD_FORMAT_VERSION);

    /* Data Offset (BE) - 0xFFFFFFFFFFFFFFFF for fixed */
    put_be64(footer + 16, VHD_DATA_OFFSET_FIX);

    /* Timestamp: seconds since 2000-01-01 UTC */
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    {
        ULARGE_INTEGER now_ft;
        now_ft.LowPart  = ft.dwLowDateTime;
        now_ft.HighPart = ft.dwHighDateTime;
        /* 2000-01-01 00:00:00 UTC = 125,911,584,000,000,000 in FILETIME (100ns units) */
        vhd_epoch_ft.QuadPart = 125911584000000000ULL;
        if (now_ft.QuadPart < vhd_epoch_ft.QuadPart)
            timestamp = 0;
        else
            timestamp = (DWORD)((now_ft.QuadPart - vhd_epoch_ft.QuadPart) / 10000000ULL);
    }
    put_be32(footer + 24, timestamp);

    /* Creator app */
    memcpy(footer + 28, VHD_CREATOR_APP, 4);

    /* Creator version (BE) */
    put_be32(footer + 32, VHD_CREATOR_VER);

    /* Creator Host OS (BE) - "Wi2k" */
    put_be32(footer + 36, VHD_HOST_OS_WIN);

    /* Original Size & Current Size (BE) */
    put_be64(footer + 40, (ULONGLONG)size_bytes);
    put_be64(footer + 48, (ULONGLONG)size_bytes);

    /* Disk Geometry (BE-ish: 2-byte cylinders BE, 1-byte heads, 1-byte spt) */
    total_sectors = size_bytes / 512;
    vhd_compute_chs(total_sectors, &cyls, &heads, &spt);
    footer[56] = (BYTE)(cyls >> 8);
    footer[57] = (BYTE)(cyls & 0xff);
    footer[58] = heads;
    footer[59] = spt;

    /* Disk Type (BE) */
    put_be32(footer + 60, VHD_DISK_TYPE_FIXED);

    /* Checksum field zeroed for the moment (offset 64..67) */

    /* Unique ID — a fresh GUID per VHD */
    if (CoCreateGuid(&unique_id) != S_OK)
        ZeroMemory(&unique_id, sizeof(unique_id));
    memcpy(footer + 68, &unique_id, 16);

    /* Saved State (offset 84) = 0; Reserved (85..511) = zero — already done */

    /* Compute checksum: one's complement of the sum of all 512 bytes
       with the checksum field treated as zero. Checksum is BE-stored. */
    sum = 0;
    for (i = 0; i < 512; i++)
        sum += footer[i];
    sum = ~sum;
    put_be32(footer + 64, sum);

    /* Append to file */
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        SetFilePointerEx(h, zero, NULL, FILE_END);
    }

    if (!WriteFile(h, footer, sizeof(footer), &bytes_written, NULL) ||
        bytes_written != sizeof(footer)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        return HRESULT_FROM_WIN32(err);
    }

    CloseHandle(h);
    return S_OK;
}

/* Use the VirtDisk API to convert a VHD file into a VHDX file.
   CreateVirtualDisk reads the source VHD's disk content and writes a
   fresh dynamic VHDX with identical sector contents. */
static HRESULT vhd_to_vhdx_convert(const wchar_t *vhd_src, const wchar_t *vhdx_dst)
{
    VIRTUAL_STORAGE_TYPE target_type;
    VIRTUAL_STORAGE_TYPE source_type;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD result;

    target_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    target_type.VendorId = VHDX_VENDOR_MS;
    source_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    source_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    /* MaximumSize=0 → take from source. BlockSizeInBytes=0 → API default
       (32 MiB), which is fine for our use case. */
    params.Version2.SourcePath = vhd_src;
    params.Version2.SourceVirtualStorageType = source_type;

    result = CreateVirtualDisk(
        &target_type,
        vhdx_dst,
        VIRTUAL_DISK_ACCESS_NONE,
        NULL,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &params,
        NULL,
        &h);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    CloseHandle(h);
    return S_OK;
}

/* Run a shell command line synchronously, suppress its output. Returns the
   process exit code, or -1 if CreateProcess failed. */
static int run_shell_quiet(const wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;
    wchar_t *cmd_buf;
    size_t len;
    HANDLE hNul = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, 0, NULL);
    if (hNul != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hNul;
        si.hStdError  = hNul;
    }

    len = wcslen(cmdline) + 1;
    cmd_buf = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!cmd_buf) {
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    wcscpy_s(cmd_buf, len, cmdline);

    if (!CreateProcessW(NULL, cmd_buf, NULL, NULL, TRUE,
                         CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(cmd_buf);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    free(cmd_buf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    return (int)exit_code;
}

/* Find the single *.img file in a directory (the cloud-image tarball
   contains exactly one .img file with a name we don't want to hardcode
   since Ubuntu's filename pattern changes per release codename). */
static BOOL find_first_img_in_dir(const wchar_t *dir, wchar_t *out_path, size_t out_chars)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];

    swprintf_s(pattern, MAX_PATH, L"%s\\*.img", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    swprintf_s(out_path, out_chars, L"%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return TRUE;
}

/* Recursive mkdir for cache dir creation. */
static void mkdir_p(const wchar_t *path)
{
    wchar_t parent[MAX_PATH];
    wchar_t *slash;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return;
    wcscpy_s(parent, MAX_PATH, path);
    slash = wcsrchr(parent, L'\\');
    if (slash && slash != parent) {
        *slash = L'\0';
        mkdir_p(parent);
    }
    CreateDirectoryW(path, NULL);
}

/* Spawn iso-patch.exe --qcow2-to-vhdx and parse its stdout for STATUS/
   PROGRESS/DEBUG/ERROR/DONE lines. The conversion (qcow2 -> raw -> VHDX
   via attach + sparse-aware copy) lives entirely in iso-patch.exe; we
   just relay its log lines into ui_log. Returns S_OK if iso-patch emitted
   `DONE:<vhdx>` and exited 0. */
static HRESULT spawn_iso_patch_qcow2_to_vhdx(const wchar_t *qcow2_in,
                                               const wchar_t *vhdx_out)
{
    wchar_t exe_dir[MAX_PATH];
    wchar_t cmdline[2048];
    wchar_t *slash;
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead = NULL, hWrite = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char    line_buf[8192];
    int     pos = 0;
    DWORD   bytes_read = 0, exit_code = (DWORD)-1;
    BOOL    done_seen = FALSE;
    wchar_t err_msg[512] = L"";
    HRESULT result;

    /* Locate iso-patch.exe next to AppSandbox.exe (same dir layout that
       iso_create_resources etc. assume). NULL means "current process exe". */
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(cmdline, 2048,
        L"\"%s\\iso-patch.exe\" --qcow2-to-vhdx \"%s\" --output \"%s\"",
        exe_dir, qcow2_in, vhdx_out);

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return HRESULT_FROM_WIN32(GetLastError());
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                         NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(hRead); CloseHandle(hWrite);
        ui_log(L"Failed to launch iso-patch.exe (error %lu)", err);
        return HRESULT_FROM_WIN32(err);
    }
    CloseHandle(hWrite);
    hWrite = NULL;

    while (ReadFile(hRead, line_buf + pos,
                     (DWORD)(sizeof(line_buf) - pos - 1),
                     &bytes_read, NULL) && bytes_read > 0) {
        int end = pos + (int)bytes_read;
        int start = 0, ci;
        line_buf[end] = '\0';
        for (ci = start; ci < end; ci++) {
            if (line_buf[ci] == '\n' || line_buf[ci] == '\r') {
                line_buf[ci] = '\0';
                if (ci > start) {
                    char *line = line_buf + start;
                    wchar_t wline[512];
                    if (strncmp(line, "STATUS:", 7) == 0) {
                        MultiByteToWideChar(CP_UTF8, 0, line + 7, -1, wline, 512);
                        ui_log(L"  %s", wline);
                    } else if (strncmp(line, "PROGRESS:", 9) == 0) {
                        /* Optional UI progress hook — for now silent so the
                           log doesn't fill with PROGRESS lines. */
                    } else if (strncmp(line, "DEBUG:", 6) == 0) {
                        MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, wline, 512);
                        ui_log(L"  [iso-patch] %s", wline);
                    } else if (strncmp(line, "ERROR:", 6) == 0) {
                        MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, err_msg, 512);
                        ui_log(L"  ERROR: %s", err_msg);
                    } else if (strncmp(line, "DONE:", 5) == 0) {
                        done_seen = TRUE;
                    }
                }
                start = ci + 1;
                if (start < end && (line_buf[start] == '\n' || line_buf[start] == '\r'))
                    start++;
            }
        }
        if (start < end) {
            memmove(line_buf, line_buf + start, end - start);
            pos = end - start;
        } else {
            pos = 0;
        }
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (done_seen && exit_code == 0) {
        result = S_OK;
    } else {
        if (err_msg[0])
            ui_log(L"iso-patch.exe failed: %s", err_msg);
        else
            ui_log(L"iso-patch.exe exited with code %lu (no DONE line)", exit_code);
        result = E_FAIL;
    }
    return result;
}

HRESULT ensure_ubuntu_cloud_image_cached(const wchar_t *version,
                                          wchar_t *out_vhdx_path,
                                          int out_max)
{
    wchar_t programdata[MAX_PATH];
    wchar_t cache_dir[MAX_PATH];
    wchar_t vhdx_path[MAX_PATH];
    wchar_t qcow2_path[MAX_PATH];
    const wchar_t *url;
    HRESULT hr;

    if (!version || !out_vhdx_path || out_max < MAX_PATH)
        return E_INVALIDARG;

    if (_wcsicmp(version, L"ubuntu-26.04") != 0) {
        ui_log(L"Error: unsupported Ubuntu version \"%s\" (only ubuntu-26.04)", version);
        return E_INVALIDARG;
    }
    url = UBUNTU_26_04_QCOW2_URL;

    if (!GetEnvironmentVariableW(L"ProgramData", programdata, MAX_PATH))
        wcscpy_s(programdata, MAX_PATH, L"C:\\ProgramData");

    swprintf_s(cache_dir, MAX_PATH, L"%s\\AppSandbox\\linux-base\\%s",
               programdata, version);
    swprintf_s(vhdx_path, MAX_PATH, L"%s\\base.vhdx", cache_dir);

    /* Already cached?  Just return the path. */
    if (GetFileAttributesW(vhdx_path) != INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(out_vhdx_path, out_max, vhdx_path);
        return S_OK;
    }

    mkdir_p(cache_dir);
    swprintf_s(qcow2_path, MAX_PATH, L"%s\\base.qcow2", cache_dir);

    /* 1. Download qcow2 from cloud-images.ubuntu.com (~600 MB). The
       download stays here (rather than in iso-patch.exe) so the cache
       is unambiguously owned by the host process and idempotent: if
       base.qcow2 already exists from a partial run, we can safely
       overwrite it — and the spawn step below operates on a stable
       on-disk file regardless. */
    if (GetFileAttributesW(qcow2_path) == INVALID_FILE_ATTRIBUTES) {
        ui_log(L"Downloading Ubuntu 26.04 cloud image (qcow2, ~600 MB)...");
        hr = URLDownloadToFileW(NULL, url, qcow2_path, 0, NULL);
        if (FAILED(hr)) {
            ui_log(L"Error: cloud image download failed (0x%08X)", hr);
            return hr;
        }
        ui_log(L"Downloaded %s", qcow2_path);
    } else {
        ui_log(L"Using previously-downloaded qcow2 at %s", qcow2_path);
    }

    /* 2. Spawn iso-patch.exe --qcow2-to-vhdx for the format conversion.
       Lives out-of-process so the work runs under SE_MANAGE_VOLUME (it
       attaches the new VHDX as a physical disk to do a sparse-aware
       byte copy). iso-patch.exe enables that privilege at startup. */
    ui_log(L"Spawning iso-patch.exe to convert qcow2 -> VHDX...");
    hr = spawn_iso_patch_qcow2_to_vhdx(qcow2_path, vhdx_path);
    if (FAILED(hr)) return hr;

    /* 3. Mark cached VHDX read-only so differencing children don't
       accidentally write through to the parent. */
    SetFileAttributesW(vhdx_path, FILE_ATTRIBUTE_READONLY);

    /* 4. Remove the qcow2 — we don't need it anymore (VHDX is the
       canonical cached artifact). */
    DeleteFileW(qcow2_path);

    ui_log(L"Ubuntu cloud image cached at %s", vhdx_path);
    wcscpy_s(out_vhdx_path, out_max, vhdx_path);
    return S_OK;
}
