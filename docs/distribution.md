# Distribution: signing, winget and Scoop

Everything here is driven by `.github/workflows/build.yml`. Pushing a tag like `v1.2.3` builds,
tests and publishes a release with the installer, the portable zip, the HeadsetControl source and
the package manager manifests below.

## Code signing

Unsigned downloads make SmartScreen say "Windows protected your PC" until enough people have run
them. A code-signing certificate removes that (an EV certificate at once, a standard one after a
while).

The workflow signs `WirelessBattery.exe` and the installer with `installer/sign.ps1` **only if** the
repository has these secrets (*Settings → Secrets and variables → Actions → New repository secret*):

| Secret | Value |
|---|---|
| `WINDOWS_SIGNING_CERT` | the `.pfx` file, base64-encoded: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx"))` in PowerShell |
| `WINDOWS_SIGNING_PASSWORD` | the `.pfx` password |

Without them the build says "No signing certificate configured" and carries on unsigned. Pull
requests from forks never see secrets, so their builds are never signed.

Where to get a certificate:
- **SignPath Foundation** signs open-source projects for free (an application is needed; they sign
  through their own GitHub Action rather than a `.pfx`, so `sign.ps1` would be swapped for their
  action).
- **Azure Trusted Signing**: inexpensive monthly plan, for individuals in supported countries.
- A commercial OV/EV certificate from a certificate authority. EV certificates now live on hardware
  tokens or cloud HSMs and cannot be exported as `.pfx`, so they also need the vendor's signing tool.

The signing path of `sign.ps1` uses `signtool` from the Windows SDK on the GitHub runner with an
RFC 3161 timestamp; it has not been run against a real certificate yet, so check the first signed
release (*right-click the exe → Properties → Digital Signatures*).

## Scoop

Every release has `wireless-battery.json`, a Scoop manifest for the portable zip with its hash.
Install straight from it:

```
scoop install https://github.com/<owner>/Wireless-Device-Battery-Monitor/releases/latest/download/wireless-battery.json
```

To get `scoop update` as well, put the file in a bucket repository (a repo with a `bucket/` folder),
then `scoop bucket add wireless-battery https://github.com/<owner>/<bucket-repo>`. The manifest has
`checkver` / `autoupdate`, so the bucket's own automation can follow new releases.

## winget

Every release has three winget manifests (schema 1.6.0): `<owner>.WirelessBattery.yaml`,
`.installer.yaml` and `.locale.en-US.yaml`. winget only installs what is in the
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) repository, so they have to be
submitted there once per release, for example with
[wingetcreate](https://github.com/microsoft/winget-create):

```
wingetcreate submit <folder with the three files>
```

Microsoft's bots validate the installer (it must install silently with `/VERYSILENT`, which Inno
Setup does). Only one package identifier per app makes sense, so this is for whoever publishes the
official releases.

Run `installer/make-manifests.ps1 -Version 1.2.3 -Repository owner/repo` to write the manifests
locally after `build-installer.ps1`.
