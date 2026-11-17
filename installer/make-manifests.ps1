# Writes winget and Scoop manifests for a release into dist\, next to the files they describe:
#   <Owner>.WirelessBattery.yaml, .installer.yaml, .locale.en-US.yaml   (winget, schema 1.6.0)
#   wireless-battery.json                                                 (Scoop)
# Run after build-installer.ps1 (and after signing: the hashes must be those of the final files).
#   ./installer/make-manifests.ps1 -Version 1.2.3 -Repository owner/Wireless-Device-Battery-Monitor
# See docs/distribution.md for how to publish them.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Repository
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root "dist"

$setup = Join-Path $dist "WirelessBattery-Setup-$Version.exe"
$zip = Join-Path $dist "WirelessBattery-$Version-portable.zip"
foreach ($f in $setup, $zip) { if (-not (Test-Path $f)) { throw "$f not found: run build-installer.ps1 first." } }

$owner = $Repository.Split("/")[0]
$id = "$owner.WirelessBattery"
$home_ = "https://github.com/$Repository"
$download = "$home_/releases/download/v$Version"
$setupSha = (Get-FileHash $setup -Algorithm SHA256).Hash
$zipSha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$description = "Battery levels of wireless mice, headsets, keyboards and controllers on the Windows taskbar."

function Write-Utf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
}

Write-Utf8 (Join-Path $dist "$id.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.6.0.schema.json
PackageIdentifier: $id
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.6.0

"@

Write-Utf8 (Join-Path $dist "$id.installer.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.6.0.schema.json
PackageIdentifier: $id
PackageVersion: $Version
InstallerType: inno
Scope: user
InstallModes:
- interactive
- silent
- silentWithProgress
InstallerSwitches:
  Custom: /CURRENTUSER
UpgradeBehavior: install
Installers:
- Architecture: x64
  InstallerUrl: $download/WirelessBattery-Setup-$Version.exe
  InstallerSha256: $setupSha
ManifestType: installer
ManifestVersion: 1.6.0

"@

Write-Utf8 (Join-Path $dist "$id.locale.en-US.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.6.0.schema.json
PackageIdentifier: $id
PackageVersion: $Version
PackageLocale: en-US
Publisher: $owner
PublisherUrl: https://github.com/$owner
PackageName: Wireless Battery
PackageUrl: $home_
License: GPL-3.0-only
LicenseUrl: $home_/blob/main/LICENSE
ShortDescription: $description
Tags:
- battery
- mouse
- headset
- controller
- taskbar
ManifestType: defaultLocale
ManifestVersion: 1.6.0

"@

$scoop = [ordered]@{
    version      = $Version
    description  = $description
    homepage     = $home_
    license      = "GPL-3.0-only"
    architecture = [ordered]@{ "64bit" = [ordered]@{ url = "$download/WirelessBattery-$Version-portable.zip"; hash = $zipSha } }
    bin          = "WirelessBattery.exe"
    shortcuts    = @(, @("WirelessBattery.exe", "Wireless Battery"))
    checkver     = [ordered]@{ github = $home_ }
    autoupdate   = [ordered]@{
        architecture = [ordered]@{ "64bit" = [ordered]@{ url = "$home_/releases/download/v`$version/WirelessBattery-`$version-portable.zip" } }
    }
}
Write-Utf8 (Join-Path $dist "wireless-battery.json") (($scoop | ConvertTo-Json -Depth 6) + "`n")

Write-Host "Wrote winget ($id) and Scoop manifests for $Version to dist\"
