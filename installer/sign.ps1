# Signs files with a code-signing certificate, if one is configured; otherwise does nothing.
#   ./installer/sign.ps1 WirelessBattery.exe dist/WirelessBattery-Setup-1.2.3.exe
#
# The certificate comes from two environment variables, which the CI workflow fills from the
# repository secrets WINDOWS_SIGNING_CERT and WINDOWS_SIGNING_PASSWORD:
#   SIGN_CERT_BASE64    the .pfx file, base64-encoded ([Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx")))
#   SIGN_CERT_PASSWORD  its password
# See docs/distribution.md for where to get a certificate.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)
$ErrorActionPreference = "Stop"

if (-not $env:SIGN_CERT_BASE64) {
    Write-Host "No signing certificate configured (WINDOWS_SIGNING_CERT secret); leaving files unsigned."
    exit 0
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found (it comes with the Windows SDK)." }

$tmp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$pfx = Join-Path $tmp "wireless-battery-signing.pfx"
[IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:SIGN_CERT_BASE64))
try {
    foreach ($pattern in $Files) {
        foreach ($f in (Resolve-Path $pattern)) {
            Write-Host "Signing $($f.Path)"
            & $signtool.FullName sign /f $pfx /p $env:SIGN_CERT_PASSWORD /fd SHA256 /td SHA256 `
                /tr "http://timestamp.digicert.com" /d "Wireless Battery" $f.Path
            if ($LASTEXITCODE -ne 0) { throw "Signing $f failed." }
        }
    }
} finally {
    Remove-Item $pfx -Force -ErrorAction SilentlyContinue
}
