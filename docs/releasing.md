# Releasing Last Download Manager

Releases are built on a developer machine; there is no CI build. Each release ships fixed file
names, so the README's `releases/latest/download/...` links always reach the newest one.

## Needs

- Visual Studio with the MSVC toolset this project pins, CMake and Ninja (see the README's build
  section) and vcpkg (`VCPKG_ROOT`).
- Node.js, for the extension build and the test fixture server.
- Inno Setup 6, for the installer. Without it the `ldm_installer` target is skipped.

## Steps

1. Set the version in `CMakeLists.txt` (`project(... VERSION X.Y.Z)`) and in `vcpkg.json`
   (`version-semver`); they must match. Commit `chore: release X.Y.Z`.
2. Close the browsers: they hold `ldm-host.exe` open, and the Release build cannot replace it then.
3. Build and test everything:

   ```
   cmake --build --preset windows-release
   ctest --preset windows-release
   ```

4. Build the packages and the installer:

   ```
   cmake --build --preset windows-release --target ldm_installer
   ```

   `build/windows-release/package/` then holds:

   | File | What it is |
   |---|---|
   | `LastDownloadManager-windows-x64-setup.exe` | the installer |
   | `LastDownloadManager-windows-x64.zip` | the portable program |
   | `LastDownloadManager-browser-extension.zip` | the extension, to load unpacked |
   | `LastDownloadManager-chromium.zip`, `LastDownloadManager-firefox.zip` | the extension packages for the stores |
   | `THIRD-PARTY-NOTICES.txt` | licenses of the shipped components |

   For ARM64 Windows, build the same target from a Visual Studio developer shell that targets
   ARM64 (`Launch-VsDevShell.ps1 -Arch arm64 -HostArch amd64`). The tests are not built, since an
   x64 machine cannot run them:

   ```
   cmake --preset windows-release-arm64
   cmake --build --preset windows-release-arm64 --target ldm_installer
   ```

   `build/windows-release-arm64/package/` then holds `LastDownloadManager-windows-arm64-setup.exe`
   and `LastDownloadManager-windows-arm64.zip`, plus the same extension files. Attach the two ARM64
   files to the release too.

5. Install the setup over the previous version on a test machine (or in Windows Sandbox with
   `installer/sandbox.wsb`) and check: LDM starts with the old download list; a browser with the
   extension hands over a download; uninstalling and answering No keeps
   `%LOCALAPPDATA%\Last Download Manager`.
6. Tag and publish (the maintainer does this):

   ```
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

   Create the GitHub release for the tag with notes grouped from the commit subjects since the
   previous tag, and attach the five files of step 4. Pre-releases are tagged `vX.Y.Z-beta.N` and
   marked as pre-release; the program's own version stays `X.Y.Z`.
7. When the extension changed, upload `LastDownloadManager-chromium.zip` to the Chrome Web Store
   and Microsoft Edge Add-ons, and `LastDownloadManager-firefox.zip` to Firefox Add-ons.

## Installing without questions

The installer is per user and needs no administrator rights.

```
LastDownloadManager-windows-x64-setup.exe /VERYSILENT /NORESTART
LastDownloadManager-windows-x64-setup.exe /VERYSILENT /NORESTART /TASKS=startup,desktopicon
```

`startup` starts LDM in the notification area when you sign in; `desktopicon` adds a desktop
shortcut. A silent uninstall (`unins000.exe /VERYSILENT` in the program folder) keeps the
download list and settings.
