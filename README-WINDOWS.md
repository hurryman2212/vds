# vDS Windows Guide

[Back to README.md](README.md)

The previous vDS Windows kernel drivers have been removed. `vdsd` now reads the
physical Bluetooth HID device directly, hides it from other applications with
HidHide, and exports the virtual controller through a local USB/IP server.
usbip-win2 imports that export as a Windows USB device.

For example, haptic feedback follows this flow:

```text
Application
  -(WASAPI render stream)-> Audio Engine [audiodg.exe / audioeng.dll]
  -(USB audio PCM)-> USB Audio class system driver [Usbaudio.sys]
  -(USB isochronous OUT URBs)-> Windows USB stack [usbip-win2]
  -(local TCP stack)-> vdsd
  -(Bluetooth HID output reports)-> HIDClass + HidBth transport minidriver [hidclass.sys + Hidbth.sys]
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

## Dependencies

### Runtime Dependencies

- Microsoft Visual C++ Redistributable
- Opus runtime DLL
- [usbip-win2](https://github.com/vadimgrn/usbip-win2)
- [HidHide](https://github.com/nefarius/HidHide)

### Build Dependencies

- `git` for version information
- CMake 3.12 or newer
- Visual Studio or Visual Studio Build Tools with the C++ toolchain
- Visual Studio vcpkg component
- Windows SDK

Install the required development tools with WinGet and run the dependency
bootstrapper from an elevated PowerShell session in the repository root:

> [!WARNING]
>
> Installing USB/IP restarts USB 3 hubs and may temporarily disconnect USB
> devices.

```powershell
winget install --exact --id Git.Git
winget install --exact --id Kitware.CMake
winget install --exact --id Microsoft.VisualStudio.2022.Community `
  --override "--passive --wait --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.Vcpkg --includeRecommended"

$DependencyDir = Join-Path $env:TEMP "vds-dependencies"
$LogPath = Join-Path $DependencyDir "install.log"
$Script = ".\packaging\windows\install_dependencies.ps1"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script `
  -Mode Download -DownloadDir $DependencyDir -LogPath $LogPath
if ($LASTEXITCODE -ne 0) {
  throw "Dependency download failed with exit code $LASTEXITCODE"
}

powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script `
  -Mode Install -DownloadDir $DependencyDir -LogPath $LogPath
if ($LASTEXITCODE -notin @(0, 3010)) {
  throw "Dependency installation failed with exit code $LASTEXITCODE"
}
if ($LASTEXITCODE -eq 3010) {
  Write-Warning "Restart Windows to complete dependency installation."
}
```

If Visual Studio or Visual Studio Build Tools is already installed, add the
desktop C++ workload and the vcpkg component through Visual Studio Installer.

The repository's `vcpkg.json` manifest restores Opus when CMake configures the
project with the vcpkg toolchain file.

## Userspace Build and Install

Build `vdsd` and `vdsctl` from a Developer PowerShell session:

```powershell
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsInstallPath = & $VsWhere `
  -latest `
  -products "*" `
  -requires Microsoft.VisualStudio.Component.Vcpkg `
  -property installationPath
if (!$VsInstallPath) {
  throw "Visual Studio with the vcpkg component was not found"
}

$VcpkgToolchain = Join-Path $VsInstallPath "VC\vcpkg\scripts\buildsystems\vcpkg.cmake"
if (!(Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf)) {
  throw "vcpkg CMake toolchain was not found: $VcpkgToolchain"
}

cmake -S . -B build\windows `
  -DINSTALL_SERVICE=YES `
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
cmake --build build\windows --config Release --parallel
```

Install the userspace tools and register `vdsd` as an automatic Windows service
from an elevated Developer PowerShell session:

```powershell
cmake --install build\windows --config Release
```

The installed files are:

```text
C:\Program Files\vDS\vdsd.exe
C:\Program Files\vDS\vdsctl.exe
C:\Program Files\vDS\opus.dll
```

The service is registered but not started immediately. Start it without
rebooting:

```powershell
sc.exe start vdsd
```

Add the install directory to `PATH` if you want to invoke the tools without
their full path:

```powershell
.\install_env.ps1 "C:\Program Files\vDS"
```

Remove the service and installed userspace files from an elevated Developer
PowerShell session:

```powershell
cmake --build build\windows --target uninstall
```
