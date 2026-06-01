<#
  test-hardware-api.ps1 - validate Hardware Dev Center API access WITHOUT exposing secrets.

  Run this in YOUR OWN PowerShell window (not through the chat / not via `! ...`),
  after filling tools\sign\partner-center.local.json.

  It checks two things and prints only PASS/FAIL lines (no token, no secret):
    AUTH         - can we get an Entra access token from the client credentials?
    PRODUCTS GET - does the token have Hardware roles (list products returns 200)?

  Paste ONLY the two result lines back.
#>
$ErrorActionPreference = 'Stop'

$cfgPath = Join-Path $PSScriptRoot 'partner-center.local.json'
if (-not (Test-Path $cfgPath)) { Write-Host "FAIL: $cfgPath not found"; exit 1 }
$c = Get-Content $cfgPath -Raw | ConvertFrom-Json
foreach ($k in 'tenantId','clientId','clientSecret') {
    if (-not $c.$k -or ($c.$k -like '<*')) {
        Write-Host "FAIL: '$k' is not filled in partner-center.local.json"; exit 1
    }
}

# 1) AUTH - client-credentials token (v1 endpoint + resource, per the Hardware API docs)
try {
    $tok = Invoke-RestMethod -Method Post `
        -Uri "https://login.microsoftonline.com/$($c.tenantId)/oauth2/token" `
        -Body @{
            grant_type    = 'client_credentials'
            client_id     = $c.clientId
            client_secret = $c.clientSecret
            resource      = 'https://manage.devcenter.microsoft.com'
        }
    if (-not $tok.access_token) { Write-Host "AUTH: FAIL (no token returned)"; exit 1 }
    Write-Host "AUTH: PASS (token acquired, expires in $($tok.expires_in)s)"
} catch {
    Write-Host "AUTH: FAIL - $($_.Exception.Message)"; exit 1
}

# 2) PRODUCTS GET - proves the Hardware roles are assigned (200 vs 403)
try {
    $r = Invoke-WebRequest -Method Get `
        -Uri "https://manage.devcenter.microsoft.com/v2.0/my/hardware/products?top=1" `
        -Headers @{ Authorization = "Bearer $($tok.access_token)" } -UseBasicParsing
    Write-Host "PRODUCTS GET: PASS (HTTP $([int]$r.StatusCode)) - Hardware roles OK"
} catch {
    $resp = $_.Exception.Response
    $code = if ($resp) { [int]$resp.StatusCode } else { 'n/a' }
    Write-Host "PRODUCTS GET: FAIL (HTTP $code) - check Hardware roles (Driver Submitter / Shipping Label owner). $($_.Exception.Message)"
}
