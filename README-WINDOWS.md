# vDS Windows Guide

[Back to README.md](README.md)

Windows uses a UdeCx virtual USB driver, a Bluetooth HID visibility filter, and
the HIDClass/HidBth Bluetooth HID stack. Games and applications should see the
vDS virtual USB controller, not the physical Bluetooth HID device.

The current architecture uses both Windows kernel drivers and the userspace
daemon for runtime communication. For example, haptic feedback currently follows
this flow:

```text
Application
  -(WASAPI render stream)-> Audio Engine [audiodg.exe / audioeng.dll]
  -(USB audio PCM)-> USB Audio class system driver [Usbaudio.sys]
  -(USB isochronous OUT URBs)-> Windows USB stack [vds_usb.sys]
  -(\\.\vdsX)-> vdsd
  -(Bluetooth HID output reports)-> HIDClass + HidBth transport minidriver [hidclass.sys + Hidbth.sys]
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

The future goal is to remove userspace from runtime communication where possible
and reduce overhead.

The virtual USB HID endpoint descriptors match each controller's USB HID
specification: DualSense HID IN/OUT use 4 ms intervals, while DualSense Edge HID
IN uses 1 ms and HID OUT uses 4 ms. vDS uses the HID IN interval to pace virtual
USB input URB completion.

## Dependencies

### Runtime Dependencies

- Microsoft Visual C++ Redistributable
- Opus runtime DLL

### Build Dependencies

- `git` (for versioning)
- `cmake` (3.12 or newer)
- Visual Studio or Visual Studio Build Tools with the C++ toolchain
- Visual Studio vcpkg component
- Windows Driver Kit
- Visual Studio Windows Driver Kit component
- Windows SDK
- Opus CMake package

Install the dependencies with `winget`; the Visual Studio vcpkg component
restores Opus from `vcpkg.json` when CMake configures the project. The driver
build script detects the installed SDK/WDK version at build time.

```powershell
winget configure --enable
winget install --exact --id Git.Git
winget install --exact --id Kitware.CMake
winget install --exact --id Microsoft.VCRedist.2015+.x64
winget install --exact --id Microsoft.VisualStudio.Community --override "--passive --add Microsoft.VisualStudio.Component.Vcpkg"
winget configure -f "https://raw.githubusercontent.com/microsoft/Windows-driver-samples/main/_wdk_utils/winget/configs/wdk-vscommunity.dsc.yaml" --accept-configuration-agreements --disable-interactivity
```

If you use an existing Visual Studio or Build Tools installation instead of the
WinGet configuration file, add the `vcpkg package manager` and
`Windows Driver Kit` individual components to that installation. The latter
installs the WDK MSBuild integration used by the driver projects.

## Driver Install

> [!CAUTION]
>
> The vDS Windows drivers are experimental. Installing or running them can cause
> unexpected system crashes or make Windows fail to boot. Use at your own risk.
>
> Open an elevated PowerShell session, enable Windows test signing, and reboot
> before installing the driver package:
>
> ```powershell
> bcdedit /set testsigning on
> shutdown /r /t 0
> ```
>
> The driver build script creates a local test code-signing certificate, trusts
> it in the Local Machine Root and TrustedPublisher stores, signs the driver
> package, and exports the public certificate with the package. The driver
> install script trusts that packaged certificate before installing the driver
> with `pnputil`; uninstall removes the corresponding trust entries. A reboot
> may still be required if Windows reports that an old driver service is pending
> deletion or a device restart cannot complete.

Windows uses one package with these driver roles:

- `vds_usb.sys`: UdeCx virtual USB root, logical ports, `\\.\vds0` through
  `\\.\vds3`, and USB child creation.
- `vds_filter.sys`: physical Bluetooth DualSense/DualSense Edge HID access
  control. Normal applications should use the vDS virtual USB device.

Build, install, or remove the Windows driver package from an elevated PowerShell
session:

```powershell
.\windrv\build.ps1
.\windrv\install.ps1
.\windrv\uninstall.ps1
```

`build.ps1` creates the signed package directories under `windrv\vds_usb` and
`windrv\vds_filter`. `install.ps1` only installs those prebuilt packages; it
does not build drivers.

`MaxPort` must be in the range `1..4`. Driver installation imports the package
default value `4` from `windrv\vds_usb\vds_usb.reg` into:

```text
HKLM\SYSTEM\CurrentControlSet\Services\vds_usb\Parameters\MaxPort
```

Changing `MaxPort` requires restarting the vDS USB root device or rebooting.
Edit `windrv\vds_usb\vds_usb.reg` before installation to change the imported
default.

## Userspace Install

Build and install the daemon and CLI with CMake:

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

cmake -S . -B build `
  -DINSTALL_SERVICE=YES `
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
cmake --build build
cmake --install build
```

Remove the installed userspace tools with CMake:

```powershell
cmake --build build --target uninstall
```

The installed files are:

```text
C:\Program Files\vDS\vdsd.exe
C:\Program Files\vDS\vdsctl.exe
C:\Program Files\vDS\opus.dll
```

When `INSTALL_SERVICE=YES` is used, CMake registers `vdsd` as an automatic
Windows service. The service is not started immediately after installation. To
start it without rebooting, run:

```powershell
sc.exe start vdsd
```

Additionally, you can add the install directory to `PATH` so the tools can be
called directly:

```powershell
.\install_env.ps1 "C:\Program Files\vDS"
```
