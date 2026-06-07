<#
  sign-mesa.ps1 -- EV Authenticode-sign the prebuilt Mesa OpenGL trio
  (opengl32.dll, gallium_wgl.dll, z-1.dll, for x64 and arm64) with the EV
  code-signing cert on the hardware token (YubiKey). Mirrors the signing
  routine in tools/sign/make-release.ps1.

  These DLLs are prebuilt (not built at release time), so they are signed
  once here after a build-mesa.cmd (re-)build, with the token inserted.
  All files go through ONE signtool call => a single PIN prompt for the batch.

  Usage (token inserted):  powershell -ExecutionPolicy Bypass -File sign-mesa.ps1
#>
[CmdletBinding()]
param(
    [string]$PrebuiltDir  = (Join-Path $PSScriptRoot '..\prebuilt'),
    [string]$Thumbprint   = '',                  # default: auto-detect the EV cert
    [string]$TimestampUrl = 'http://ts.ssl.com'  # SSL.com RFC-3161 timestamp
)
$ErrorActionPreference = 'Stop'

# Self-contained Authenticode signing -- no driver-attestation config involved.
$tsUrl = $TimestampUrl
$thumb = $Thumbprint

# --- signtool (latest Windows 10/11 SDK, x64) ---
$st = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
      Sort-Object FullName -Descending | Select-Object -First 1
if (-not $st) { throw "signtool.exe not found under Windows Kits\10\bin\*\x64." }
$signtool = $st.FullName

# --- EV code-signing cert (same selection as make-release.ps1: a CA-issued
#     code-signing cert with a private key in CurrentUser\My) ---
if (-not $thumb) {
    $now = Get-Date
    $cands = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.HasPrivateKey -and ($_.Issuer -ne $_.Subject) -and ($_.NotBefore -le $now) -and ($_.NotAfter -gt $now) })
    if ($cands.Count -eq 0) { throw "No usable code-signing cert in CurrentUser\My (is the YubiKey inserted?)." }
    if ($cands.Count -gt 1) { throw "Multiple code-signing certs found; set evCertThumbprint in tools/sign/submission-config.json." }
    $thumb = $cands[0].Thumbprint
}
$cert = Get-Item "Cert:\CurrentUser\My\$thumb" -ErrorAction Stop
Write-Host ("Signing cert : {0}  [{1}]" -f $cert.Subject, $thumb)
Write-Host ("Timestamp    : {0}" -f $tsUrl)
Write-Host ("signtool     : {0}" -f $signtool)

# --- the six DLLs ---
$files = @()
foreach ($arch in 'x64','arm64') {
    foreach ($name in 'opengl32.dll','gallium_wgl.dll','z-1.dll') {
        $p = Join-Path $PrebuiltDir "$arch\$name"
        if (Test-Path $p) { $files += (Resolve-Path $p).Path } else { Write-Warning "missing (skipped): $p" }
    }
}
if (-not $files) { throw "No Mesa DLLs found under $PrebuiltDir (run build-mesa.cmd first)." }
Write-Host "`nFiles to sign:"; $files | ForEach-Object { Write-Host "  $_" }

# --- sign (one call = one PIN), then verify ---
Write-Host "`n>> signtool sign (enter the token PIN when prompted)..."
& $signtool sign /sha1 $thumb /fd SHA256 /tr $tsUrl /td SHA256 /v @files
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)." }

Write-Host "`n>> signtool verify..."
& $signtool verify /pa @files
if ($LASTEXITCODE -ne 0) { throw "signtool verify failed (exit $LASTEXITCODE)." }

Write-Host "`nAll Mesa DLLs EV-signed and verified."
