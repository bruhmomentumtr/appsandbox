<#
  sign-drivers.ps1 - EV-sign + Microsoft attestation-sign the AppSandbox drivers.

  This (for VDD + VAD):
    1. EV-signs the driver binaries (.sys/.dll) in ONE batched call - skipped if they were
       already signed (e.g. by make-release's all-binaries pass),
    2. regenerates each catalog so it hashes the EV-signed binary, then EV-signs BOTH
       catalogs in ONE call (so the package submitted to Microsoft is fully EV-signed),
    3. builds a submission CAB per driver (each containing the EV-signed binary + catalog),
    4. EV-signs ALL cabs in ONE call  => three PIN prompts total: binaries, catalogs, cabs,
    5. creates a Hardware Dev Center product (testHarness = Attestation) per driver,
    6. creates a submission, uploads the signed cab to the returned SAS URL, commits,
    7. WAITS for Microsoft to finish signing BOTH in parallel, downloading each into
       <outDir>\<cabFolder> the moment it completes.

  Microsoft's signing can take a long time (minutes to hours; longer if a
  submission enters manual review), and the access token only lives 60 min.
  So the wait is built to last:
    - polls until the submission passes or fails, with NO timeout by default,
    - auto-refreshes the access token across the whole wait,
    - retries transient / throttling (429) / 5xx errors instead of aborting,
    - records each committed submission so an interrupted wait can be resumed
      with -Resume (re-attaches to the live submission instead of re-submitting).

  PREREQS:
    - tools\sign\partner-center.local.json filled + validated (test-hardware-api.ps1 => PASS).
    - Drivers built Release|x64 (tools\vdd + tools\vad WDK output present).
    - YubiKey with the App Sandbox LLC EV cert inserted (three PINs: binaries, cats, cabs).

  USAGE:
    .\sign-drivers.ps1                  # build cabs, EV-sign, submit, wait, download
    .\sign-drivers.ps1 -Resume          # re-attach to the last submissions and finish
    .\sign-drivers.ps1 -NoCab -NoSign   # reuse existing signed cabs (re-submit)
    .\sign-drivers.ps1 -SkipSubmit      # only build + EV-sign the cabs (no upload)
    .\sign-drivers.ps1 -TimeoutMinutes 0  # 0 (default) = wait indefinitely
#>
[CmdletBinding()]
param(
  [string]$ConfigPath   = (Join-Path $PSScriptRoot 'submission-config.json'),
  [string]$CredPath     = (Join-Path $PSScriptRoot 'partner-center.local.json'),
  [switch]$NoCab,
  [switch]$NoSign,
  [switch]$SkipSubmit,
  [switch]$Resume,
  [int]$PollSeconds     = 30,
  [int]$TimeoutMinutes  = 0       # 0 = wait indefinitely (the build must not give up on Microsoft)
)

$ErrorActionPreference = 'Stop'
$ApiBase = 'https://manage.devcenter.microsoft.com/v2.0/my/hardware'

# ---------------------------------------------------------------- helpers ----

function Resolve-RelPath([string]$p) {
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    return [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $p))
}

function Find-SignTool {
    $c = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $c) { throw "signtool.exe not found under Windows Kits\10\bin\*\x64." }
    return $c.FullName
}

function Find-Inf2Cat {
    $c = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x86\Inf2Cat.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $c) { throw "Inf2Cat.exe not found under Windows Kits\10\bin\*\x86." }
    return $c.FullName
}

# EV-sign one or more files in a single signtool call (one PIN, thanks to YubiKey PIN caching).
function Invoke-EvSign([string[]]$files, [string]$what) {
    if (-not $files -or $files.Count -eq 0) { return }
    Write-Host "  EV-signing $what ($($files.Count) file(s))..."
    & $script:SignTool sign /sha1 $script:Thumb /fd SHA256 /tr $script:TsUrl /td SHA256 @files | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "EV-signing $what failed (exit $LASTEXITCODE)." }
}

# A smart-card cert stays in the store with HasPrivateKey=$true even after the YubiKey is unplugged,
# so HasPrivateKey is not proof the token is present - and we must NOT probe the key to find out:
# opening it (GetECDsaPrivateKey / signtool) asks the CNG layer to locate the card and pops a Windows
# "insert smart card" dialog when it's gone. Two UI-free checks instead:
#   AnyCardPresent()    - WinSCard status query: is any smart card in any reader at all?
#   KeyOnPresentCard()  - opens THIS cert's own key container with NCRYPT_SILENT_FLAG, so the KSP
#                         returns an error (never UI) when the specific card bearing it is absent.
#                         This is what tells our token apart from some unrelated card the user inserted.
if (-not ([System.Management.Automation.PSTypeName]'AsbScReader').Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class AsbScReader {
    [DllImport("winscard.dll")] static extern int SCardEstablishContext(uint scope, IntPtr r1, IntPtr r2, out IntPtr ctx);
    [DllImport("winscard.dll")] static extern int SCardReleaseContext(IntPtr ctx);
    [DllImport("winscard.dll", CharSet=CharSet.Unicode, EntryPoint="SCardListReadersW")]
    static extern int SCardListReaders(IntPtr ctx, string groups, [Out] char[] readers, ref int len);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct RDR { public string szReader; public IntPtr pv; public uint cur; public uint evt; public uint cbAtr;
                 [MarshalAs(UnmanagedType.ByValArray, SizeConst=36)] public byte[] atr; }
    [DllImport("winscard.dll", CharSet=CharSet.Unicode, EntryPoint="SCardGetStatusChangeW")]
    static extern int SCardGetStatusChange(IntPtr ctx, uint timeout, [In,Out] RDR[] states, uint count);
    const uint SCOPE_USER = 0; const uint STATE_PRESENT = 0x20;
    public static bool AnyCardPresent() {
        IntPtr ctx;
        if (SCardEstablishContext(SCOPE_USER, IntPtr.Zero, IntPtr.Zero, out ctx) != 0) return false;
        try {
            int len = 0;
            if (SCardListReaders(ctx, null, null, ref len) != 0 || len <= 1) return false;
            char[] buf = new char[len];
            if (SCardListReaders(ctx, null, buf, ref len) != 0) return false;
            var rs = new List<string>(); int s = 0;
            for (int i = 0; i < buf.Length; i++) {
                if (buf[i] == '\0') { if (i > s) rs.Add(new string(buf, s, i - s)); s = i + 1;
                    if (i + 1 >= buf.Length || buf[i+1] == '\0') break; } }
            if (rs.Count == 0) return false;
            var st = new RDR[rs.Count];
            for (int i = 0; i < rs.Count; i++) { st[i].szReader = rs[i]; st[i].cur = 0; st[i].atr = new byte[36]; }
            if (SCardGetStatusChange(ctx, 0, st, (uint)st.Length) != 0) return false;
            foreach (var r in st) if ((r.evt & STATE_PRESENT) != 0) return true;
            return false;
        } finally { SCardReleaseContext(ctx); }
    }
    [DllImport("crypt32.dll", SetLastError=true)]
    static extern bool CertGetCertificateContextProperty(IntPtr ctx, uint propId, IntPtr pvData, ref uint cb);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct KEYPROV { public string container; public string prov; public uint provType; public uint flags; public uint cParam; public IntPtr rgParam; public uint keySpec; }
    [DllImport("ncrypt.dll", CharSet=CharSet.Unicode)] static extern int NCryptOpenStorageProvider(out IntPtr prov, string name, uint flags);
    [DllImport("ncrypt.dll", CharSet=CharSet.Unicode)] static extern int NCryptOpenKey(IntPtr prov, out IntPtr key, string keyName, uint legacyKeySpec, uint flags);
    [DllImport("ncrypt.dll")] static extern int NCryptFreeObject(IntPtr h);
    const uint KEYPROV_PROP = 2; const uint SILENT = 0x40;
    public static bool KeyOnPresentCard(IntPtr certContext) {
        uint cb = 0;
        if (!CertGetCertificateContextProperty(certContext, KEYPROV_PROP, IntPtr.Zero, ref cb) || cb == 0) return false;
        IntPtr buf = Marshal.AllocHGlobal((int)cb);
        try {
            if (!CertGetCertificateContextProperty(certContext, KEYPROV_PROP, buf, ref cb)) return false;
            var kp = (KEYPROV)Marshal.PtrToStructure(buf, typeof(KEYPROV));
            if (string.IsNullOrEmpty(kp.prov) || string.IsNullOrEmpty(kp.container)) return false;
            IntPtr prov;
            if (NCryptOpenStorageProvider(out prov, kp.prov, 0) != 0) return false;
            try {
                IntPtr key;
                if (NCryptOpenKey(prov, out key, kp.container, kp.keySpec, SILENT) == 0) { NCryptFreeObject(key); return true; }
                return false;
            } finally { NCryptFreeObject(prov); }
        } finally { Marshal.FreeHGlobal(buf); }
    }
}
'@
}
function Test-SmartCardPresent { try { return [AsbScReader]::AnyCardPresent() } catch { return $false } }
function Test-CertOnPresentCard($cert) { try { return [AsbScReader]::KeyOnPresentCard($cert.Handle) } catch { return $false } }

function Resolve-SigningThumbprint([string]$configured) {
    if (-not (Test-SmartCardPresent)) { throw "No signing token detected (is the YubiKey inserted?)." }
    if ($configured) {
        $c = Get-Item "Cert:\CurrentUser\My\$configured" -ErrorAction SilentlyContinue
        if ($c -and $c.HasPrivateKey -and (Test-CertOnPresentCard $c)) { return $c.Thumbprint }
        throw "evCertThumbprint '$configured' is not on a present smart card (is the YubiKey inserted?)."
    }
    $now = Get-Date
    $cands = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.HasPrivateKey -and ($_.Issuer -ne $_.Subject) -and ($_.NotBefore -le $now) -and ($_.NotAfter -gt $now) -and (Test-CertOnPresentCard $_) })
    if ($cands.Count -eq 0) { throw "No code-signing cert on a present smart card found (is the YubiKey inserted?)." }
    if ($cands.Count -gt 1) { throw "Multiple code-signing certs found; set 'evCertThumbprint' in submission-config.json." }
    Write-Host ("Signing cert: {0}  [{1}]" -f $cands[0].Subject, $cands[0].Thumbprint)
    return $cands[0].Thumbprint
}

function Find-SourceFile([string]$sourceDir, [string]$leaf) {
    $direct = Join-Path $sourceDir $leaf
    if (Test-Path $direct) { return (Resolve-Path $direct).Path }
    $hit = Get-ChildItem $sourceDir -Recurse -Filter $leaf -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

# Read the version (single source of truth) from the repo-root Directory.Build.props.
function Get-AsbVersion {
    $props = Resolve-RelPath '..\..\Directory.Build.props'
    if (-not (Test-Path $props)) { throw "Directory.Build.props not found at $props" }
    [xml]$x = Get-Content $props -Raw
    $pg = @($x.Project.PropertyGroup) | Where-Object { $_.AsbVersionMajor } | Select-Object -First 1
    if (-not $pg) { throw "AsbVersion* not found in Directory.Build.props." }
    $p = @("$($pg.AsbVersionMajor)", "$($pg.AsbVersionMinor)", "$($pg.AsbVersionPatch)", "$($pg.AsbVersionRevision)") | ForEach-Object { $_.Trim() }
    return [pscustomobject]@{ Short = ($p[0..2] -join '.'); Full = ($p -join '.') }
}

# Token cache (auto-refresh 5 min before expiry - survives multi-hour waits).
$script:Tok = $null; $script:TokExp = [datetime]::MinValue
function Get-Token {
    if ($script:Tok -and ((Get-Date) -lt $script:TokExp)) { return $script:Tok }
    if (-not (Test-Path $CredPath)) { throw "Credentials file not found: $CredPath" }
    $cred = Get-Content $CredPath -Raw | ConvertFrom-Json
    foreach ($k in 'tenantId','clientId','clientSecret') {
        if (-not $cred.$k -or ($cred.$k -like '<*')) { throw "'$k' not filled in $CredPath." }
    }
    $r = Invoke-RestMethod -Method Post -Uri "https://login.microsoftonline.com/$($cred.tenantId)/oauth2/token" -Body @{
        grant_type='client_credentials'; client_id=$cred.clientId; client_secret=$cred.clientSecret
        resource='https://manage.devcenter.microsoft.com'
    }
    $script:Tok = $r.access_token
    $script:TokExp = (Get-Date).AddSeconds([math]::Max([int]$r.expires_in - 300, 60))
    return $script:Tok
}

# Hardware API call that surfaces the server error body on failure.
function Invoke-Api([string]$method, [string]$uri, $body) {
    $params = @{
        Method      = $method; Uri = $uri
        Headers     = @{ Authorization = "Bearer $(Get-Token)"; Accept = 'application/json' }
        ContentType = 'application/json'   # required even on the bodiless /commit POST (else HTTP 415)
        Body        = $(if ($null -ne $body) { $body | ConvertTo-Json -Depth 8 } else { '' })
    }
    try { return Invoke-RestMethod @params }
    catch {
        $detail = ''
        $resp = $_.Exception.Response
        if ($resp -and $resp.GetResponseStream) {
            try { $detail = (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() } catch {}
        }
        throw "API $method $uri failed: $($_.Exception.Message)`n$detail"
    }
}

# Resilient GET used while polling - never gives up on transient failures.
function Get-SubmissionResilient([string]$productId, [string]$submissionId) {
    $uri = "$ApiBase/products/$productId/submissions/$submissionId"
    $attempt = 0
    while ($true) {
        $attempt++
        try {
            return Invoke-RestMethod -Method Get -Uri $uri -Headers @{ Authorization = "Bearer $(Get-Token)"; Accept = 'application/json' }
        } catch {
            $code = 0
            if ($_.Exception.Response) { try { $code = [int]$_.Exception.Response.StatusCode } catch {} }
            $retryable = ($code -eq 0) -or ($code -eq 401) -or ($code -eq 429) -or ($code -ge 500)
            if (-not $retryable) { throw "Poll GET failed (HTTP $code): $($_.Exception.Message)" }
            if ($code -eq 401) { $script:Tok = $null }   # force a token refresh
            $wait = [math]::Min(120, 15 * [math]::Min($attempt, 8))
            Write-Host "      (transient poll error HTTP $code - retrying in ${wait}s)"
            Start-Sleep -Seconds $wait
        }
    }
}

# Poll a SET of committed submissions concurrently in one loop (so VDD + VAD wait in
# parallel; total wait = the slowest, not the sum). Each is downloaded the moment it
# completes. No timeout unless asked. A failure is recorded (not thrown) so a sibling
# still finishes; the caller decides what to do with FAILED rows.
#   $items: array of [pscustomobject]@{ Driver=<cfg driver>; ProductId; SubmissionId }
function Wait-ForAllSigning($items) {
    $items = @($items)
    if ($items.Count -eq 0) { return @() }
    $start = Get-Date
    $deadline = if ($TimeoutMinutes -gt 0) { $start.AddMinutes($TimeoutMinutes) } else { $null }
    $track = @{}
    foreach ($it in $items) { $track[$it.SubmissionId] = [pscustomobject]@{ LastKey=''; LastBeat=$start; Done=$false; Result=$null } }

    while ($true) {
        $anyLeft = $false
        foreach ($it in $items) {
            $tr = $track[$it.SubmissionId]
            if ($tr.Done) { continue }
            $s = Get-SubmissionResilient $it.ProductId $it.SubmissionId
            $state = $s.workflowStatus.state
            $step  = $s.workflowStatus.currentStep
            $now = Get-Date
            $key = "$step/$state"
            if (($key -ne $tr.LastKey) -or (($now - $tr.LastBeat).TotalMinutes -ge 2)) {
                Write-Host ("    [{0}] {1} / {2}  (elapsed {3}m)" -f $it.Driver.cabFolder, $step, $state, [int]($now - $start).TotalMinutes)
                [Console]::Out.Flush()   # push the line out now - MSBuild's Exec can buffer PS output
                $tr.LastKey = $key; $tr.LastBeat = $now
            }
            if ($state -eq 'completed') {
                $dst = Save-SignedPackage $s $it.Driver.cabFolder
                $tr.Result = [pscustomobject]@{ Product=$it.Driver.productName; ProductId=$it.ProductId; SubmissionId=$it.SubmissionId; Output=$dst }
                $tr.Done = $true
            } elseif ($state -eq 'failed') {
                $msg = [string]::Join('; ', @($s.workflowStatus.messages))
                Write-Warning "Signing FAILED at '$step' for $($it.Driver.cabFolder): $msg"
                $tr.Result = [pscustomobject]@{ Product=$it.Driver.productName; ProductId=$it.ProductId; SubmissionId=$it.SubmissionId; Output="FAILED: $msg" }
                $tr.Done = $true
            } else {
                $anyLeft = $true
            }
        }
        if (-not $anyLeft) { break }
        if ($deadline -and ((Get-Date) -gt $deadline)) {
            throw "Hit -TimeoutMinutes ($TimeoutMinutes); submission(s) still live. Re-run with -Resume to keep waiting."
        }
        Start-Sleep -Seconds $PollSeconds
    }
    return @($items | ForEach-Object { $track[$_.SubmissionId].Result })
}

function Get-StatePath([string]$workDir, $d) { return (Join-Path $workDir "$($d.cabFolder).submission.json") }

function Save-SignedPackage($submission, [string]$cabFolder) {
    $signedUrl = ($submission.downloads.items | Where-Object { $_.type -eq 'signedPackage' } | Select-Object -First 1).url
    if (-not $signedUrl) { throw "No signedPackage URL in completed submission for $cabFolder." }
    $zip = Join-Path $script:OutDir "$cabFolder-signed.zip"
    $dst = Join-Path $script:OutDir $cabFolder
    Invoke-WebRequest -Uri $signedUrl -OutFile $zip -UseBasicParsing
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $dst -Force
    Write-Host "  MS-signed package -> $dst"
    return $dst
}

# True if a file is already Authenticode-signed by OUR EV cert (so we can skip re-signing
# it - e.g. when make-release already signed it). Uses the cmdlet, not signtool, so an
# untrusted/unsigned file returns cleanly instead of throwing under ErrorActionPreference=Stop.
function Test-IsEvSigned([string]$path) {
    $sig = Get-AuthenticodeSignature -LiteralPath $path
    return ($null -ne $sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $script:Thumb)
}

# Locate a driver's compiled binary (.dll for the UMDF VDD, .sys for the KMDF VAD).
function Get-DriverBinary($d) {
    $srcDir = Resolve-RelPath $d.sourceDir
    foreach ($ext in '.dll', '.sys') {
        $c = Find-SourceFile $srcDir ("{0}{1}" -f $d.cabFolder, $ext)
        if ($c) { return $c }
    }
    throw "Driver binary (.sys/.dll) for $($d.cabFolder) not found in $srcDir."
}

# Regenerate a driver's catalog so it hashes the (already EV-signed) binary, writing it back
# into bin\Release\drivers. It is EV-signed immediately after (batched with the other catalog
# in one call) so the package submitted to Microsoft carries OUR EV-signed catalog. inf2cat
# needs exactly one INF per directory, so the regeneration runs in a temp dir.
function Update-DriverCatalog($d, [string]$workDir) {
    $srcDir = Resolve-RelPath $d.sourceDir
    $inf = Find-SourceFile $srcDir ("{0}.inf" -f $d.cabFolder)
    if (-not $inf) { throw "INF for $($d.cabFolder) not found in $srcDir." }
    $bin = Get-DriverBinary $d
    $binLeaf = Split-Path $bin -Leaf

    $tmp = Join-Path $workDir "_sign_$($d.cabFolder)"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Copy-Item $inf (Join-Path $tmp (Split-Path $inf -Leaf)) -Force
    Copy-Item $bin (Join-Path $tmp $binLeaf) -Force

    Write-Host "  regenerating catalog ($($d.cabFolder), os=$($cfg.inf2catOs))..."
    & $script:Inf2Cat /driver:"$tmp" /os:$($cfg.inf2catOs) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed for $($d.cabFolder) (os=$($cfg.inf2catOs))." }
    $cat = Get-ChildItem $tmp -Filter *.cat -File | Select-Object -First 1
    if (-not $cat) { throw "Inf2Cat produced no catalog for $($d.cabFolder)." }
    $dst = Join-Path $srcDir $cat.Name
    Copy-Item $cat.FullName $dst -Force
    return $dst   # the regenerated catalog in bin\Release\drivers, to be EV-signed in Phase 0
}

# Build a submission cab from a driver's (already EV-signed) files in bin\Release\drivers.
function New-DriverCab($d, [string]$workDir) {
    $srcDir = Resolve-RelPath $d.sourceDir
    if (-not (Test-Path $srcDir)) { throw "Source dir for $($d.cabFolder) not found: $srcDir (build the driver Release|x64 first)." }

    $contents = @()
    foreach ($leaf in $d.required) {
        $f = Find-SourceFile $srcDir $leaf
        if (-not $f) { throw "Required file '$leaf' not found under $srcDir for $($d.cabFolder)." }
        $contents += $f
    }
    foreach ($leaf in $d.optional) {
        $f = Find-SourceFile $srcDir $leaf
        if (-not $f -and $script:SymbolDir) { $f = Find-SourceFile $script:SymbolDir $leaf }
        if ($f) { $contents += $f } else { Write-Host "  note: optional '$leaf' not found (skipping)." }
    }

    $ddf = @(
        '.OPTION EXPLICIT'
        '.Set CabinetFileCountThreshold=0'; '.Set FolderFileCountThreshold=0'; '.Set FolderSizeThreshold=0'
        '.Set MaxCabinetSize=0'; '.Set MaxDiskFileCount=0'; '.Set MaxDiskSize=0'
        '.Set CompressionType=MSZIP'; '.Set Cabinet=on'; '.Set Compress=on'
        ".Set CabinetNameTemplate=$($d.cabName)"
        ".Set DiskDirectoryTemplate=`"$workDir`""
        ".Set DestinationDir=$($d.cabFolder)"
        ".Set RptFileName=`"$workDir\setup.rpt`""    # keep makecab's byproducts out of the repo root
        ".Set InfFileName=`"$workDir\setup.inf`""
    )
    foreach ($f in $contents) { $ddf += "`"$f`"" }

    $ddfPath = Join-Path $workDir "$($d.cabFolder).ddf"
    Set-Content -Path $ddfPath -Value $ddf -Encoding ASCII

    Write-Host "Building cab $($d.cabName) ($($contents.Count) files)..."
    & makecab.exe /f $ddfPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makecab failed for $($d.cabFolder) (exit $LASTEXITCODE)." }

    $cab = Get-ChildItem $workDir -Recurse -Filter $d.cabName -File | Select-Object -First 1
    if (-not $cab) { throw "Cab $($d.cabName) not produced under $workDir." }
    return $cab.FullName
}

# ------------------------------------------------------------------- main ----

if (-not (Test-Path $ConfigPath)) { throw "Config not found: $ConfigPath" }
$cfg            = Get-Content $ConfigPath -Raw | ConvertFrom-Json
$asb            = Get-AsbVersion          # version from Directory.Build.props (single source)
$workDir          = Resolve-RelPath $cfg.workDir
$script:OutDir    = Resolve-RelPath $cfg.outDir
$script:SymbolDir = if ($cfg.symbolDir) { Resolve-RelPath $cfg.symbolDir } else { $null }
New-Item -ItemType Directory -Force -Path $workDir, $script:OutDir | Out-Null
$results = @()

# ---- Resume: re-attach to in-flight submissions and finish (parallel) ----
if ($Resume) {
    Write-Host "Resume: re-attaching to in-flight submissions (parallel, no timeout)..."
    $pending = @()
    foreach ($d in $cfg.drivers) {
        $statePath = Get-StatePath $workDir $d
        if (-not (Test-Path $statePath)) { Write-Warning "No submission state for $($d.cabFolder) ($statePath) - skipping."; continue }
        $st = Get-Content $statePath -Raw | ConvertFrom-Json
        Write-Host "  re-attaching $($d.cabFolder): submission $($st.submissionId)"
        $pending += [pscustomobject]@{ Driver = $d; ProductId = "$($st.productId)"; SubmissionId = "$($st.submissionId)" }
    }
    $results = Wait-ForAllSigning $pending
    Write-Host "`n==================== DONE (resume) ===================="
    $results | Format-Table -AutoSize
    $failed = @($results | Where-Object { "$($_.Output)" -like 'FAILED:*' })
    if ($failed.Count) { throw "$($failed.Count) submission(s) failed Microsoft signing - see above." }
    return
}

# ---- Resolve signing tools up front (YubiKey required). ----
$needSign = (-not $NoCab) -or (-not $NoSign)
if ($needSign) {
    $script:SignTool = Find-SignTool
    $script:Inf2Cat  = Find-Inf2Cat
    $script:Thumb    = Resolve-SigningThumbprint $cfg.evCertThumbprint
    $script:TsUrl    = $cfg.timestampUrl
}

# ---- Phase 0: EV-sign driver binaries + regenerate & EV-sign their catalogs ----
# Batched to ONE PIN each: EV-sign BOTH binaries in one call (skipped if make-release already
# signed them in its all-binaries pass), regenerate the catalogs, then EV-sign BOTH catalogs
# in one call. This makes the package SUBMITTED to Microsoft fully EV-signed - binary AND
# catalog. The cabs are EV-signed in Phase 2. Driver PIN prompts: binaries, catalogs, cabs.
if (-not $NoCab) {
    $bins = @($cfg.drivers | ForEach-Object { Get-DriverBinary $_ })
    $unsigned = @($bins | Where-Object { -not (Test-IsEvSigned $_) })
    if ($unsigned.Count) { Invoke-EvSign $unsigned "driver binaries (sys/dll)" }
    else { Write-Host "Driver binaries already EV-signed (skipping binary PIN)." }
    $cats = @($cfg.drivers | ForEach-Object { Update-DriverCatalog $_ $workDir })
    Invoke-EvSign $cats "driver catalogs"
}

# ---- Phase 1: cab each (now EV-signed) driver ----
$jobs = @()
foreach ($d in $cfg.drivers) {
    $cabPath = Join-Path $workDir $d.cabName
    if (-not $NoCab) { $cabPath = New-DriverCab $d $workDir }
    elseif (-not (Test-Path $cabPath)) { throw "-NoCab set but cab not found: $cabPath" }
    $jobs += [pscustomobject]@{ Driver = $d; Cab = $cabPath }
}

# ---- Phase 2: EV-sign all cabs in ONE signtool call (one PIN) ----
# The cabs already contain EV-signed binaries + EV-signed catalogs (Phase 0). This signs the
# cab envelopes, which authenticate the submission to Microsoft.
if (-not $NoSign) {
    $cabs = @($jobs | ForEach-Object { $_.Cab })
    Write-Host "EV-signing $($cabs.Count) cab(s) - enter the YubiKey PIN..."
    & $script:SignTool sign /sha1 $script:Thumb /fd SHA256 /tr $script:TsUrl /td SHA256 /v @cabs
    if ($LASTEXITCODE -ne 0) { throw "signtool sign (cabs) failed (exit $LASTEXITCODE)." }
    & $script:SignTool verify /pa @cabs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cab signature verify failed (exit $LASTEXITCODE)." }
    Write-Host "Cabs EV-signed + verified."
}

if ($SkipSubmit) { Write-Host "`n-SkipSubmit: stopping after EV-signing. Cabs in $workDir."; return }

# ---- Phase 3: submit ALL drivers, then wait for Microsoft in PARALLEL ----
# Each version must be its own product (the API allows only one submission per product),
# so the version goes in the product name to make the dashboard a readable history.
$ver = $asb.Short

# Phase 3a: create product + submission + upload + commit for every driver (quick).
$pending = @()
foreach ($j in $jobs) {
    $d = $j.Driver
    $productDisplayName = "$($d.productName) $ver"
    Write-Host "`n=== $productDisplayName ==="

    $product = Invoke-Api POST "$ApiBase/products" @{
        productName          = $productDisplayName
        testHarness          = 'Attestation'
        announcementDate     = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss')
        deviceMetadataIds    = @()
        firmwareVersion      = $asb.Full
        deviceType           = $cfg.deviceType
        isTestSign           = $false
        isFlightSign         = $false
        marketingNames       = @()
        selectedProductTypes = $cfg.selectedProductTypes
        requestedSignatures  = $cfg.requestedSignatures
        additionalAttributes = @{}
    }
    $productId = "$($product.id)"
    Write-Host "  product id: $productId"

    $submission = Invoke-Api POST "$ApiBase/products/$productId/submissions" @{
        name = "$($d.cabFolder)-$ver"; type = 'initial'
    }
    $submissionId = "$($submission.id)"
    $uploadUrl = ($submission.downloads.items | Where-Object { $_.type -eq 'initialPackage' } | Select-Object -First 1).url
    if (-not $uploadUrl) { throw "No initialPackage upload URL returned for $($d.productName)." }
    Write-Host "  submission id: $submissionId - uploading cab..."

    Invoke-RestMethod -Method Put -Uri $uploadUrl -InFile $j.Cab -Headers @{ 'x-ms-blob-type' = 'BlockBlob' } | Out-Null
    Invoke-Api POST "$ApiBase/products/$productId/submissions/$submissionId/commit" $null | Out-Null

    # Persist state BEFORE the long wait so Ctrl+C / a crash can resume with -Resume.
    @{ productId = $productId; submissionId = $submissionId; productName = $d.productName; name = $submission.name } |
        ConvertTo-Json | Set-Content -Path (Get-StatePath $workDir $d) -Encoding UTF8
    Write-Host "  committed."

    $pending += [pscustomobject]@{ Driver = $d; ProductId = $productId; SubmissionId = $submissionId }
}

# Phase 3b: poll every committed submission at once - they sign in parallel.
Write-Host "`nAll $($pending.Count) submission(s) committed - waiting for Microsoft in PARALLEL (no timeout). Safe to Ctrl+C and resume with -Resume."
[Console]::Out.Flush()
$results = Wait-ForAllSigning $pending

Write-Host "`n==================== DONE ===================="
$results | Format-Table -AutoSize
Write-Host "Microsoft-signed drivers are under: $script:OutDir"
$failed = @($results | Where-Object { "$($_.Output)" -like 'FAILED:*' })
if ($failed.Count) { throw "$($failed.Count) submission(s) failed Microsoft signing - see above." }
