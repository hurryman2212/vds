# SPDX-License-Identifier: MIT

param(
  [string]$OutputDir = "",
  [string]$ToolsDir = "",
  [string]$PkgRel = "",
  [string]$Arch = "x64",
  [string]$Wix = ""
)

$ErrorActionPreference = "Stop"

if ($Arch -ne "x64") {
  throw "the Windows USB/IP and HidHide dependency stack only supports -Arch x64"
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$WixDir = Join-Path $ScriptDir "wix"
$GeneratedDir = Join-Path $ScriptDir "generated"

. (Join-Path $RepoRoot "generate_version.ps1")

function Resolve-VdsPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return [System.IO.Path]::GetFullPath($Path)
}

function Resolve-VdsOutputDir {
  if (![string]::IsNullOrWhiteSpace($OutputDir)) {
    return Resolve-VdsPath -Path $OutputDir
  }
  return Join-Path $RepoRoot "dist\windows"
}

function Resolve-VdsMsiVersion {
  $TagInfo = Get-VdsVersionTagInfoFromGit -RepoRoot $RepoRoot
  if (!$TagInfo) {
    Write-Warning "Falling back to MSI version 0.1.0"
    return "0.1.0"
  }

  $Parts = @(Get-VdsSemverParts -TagName $TagInfo.Tag)
  return "$($Parts[0]).$($Parts[1]).$($Parts[2])"
}

function Resolve-Wix {
  if (![string]::IsNullOrWhiteSpace($Wix)) {
    return Resolve-VdsPath -Path $Wix
  }

  $Command = Get-Command wix.exe -ErrorAction SilentlyContinue
  if ($Command) {
    return $Command.Source
  }

  throw "wix.exe was not found. Install the WiX Toolset CLI and ensure wix.exe is in PATH, or pass -Wix."
}

function Test-VdsToolsDir {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if ([string]::IsNullOrWhiteSpace($Path)) {
    return $false
  }

  $RequiredFiles = @("vdsd.exe", "vdsctl.exe", "opus.dll")
  foreach ($FileName in $RequiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $Path $FileName) -PathType Leaf)) {
      return $false
    }
  }
  return $true
}

function Resolve-VdsToolsDir {
  if (![string]::IsNullOrWhiteSpace($ToolsDir)) {
    $Resolved = Resolve-VdsPath -Path $ToolsDir
    if (!(Test-VdsToolsDir -Path $Resolved)) {
      throw "tools directory must contain vdsd.exe, vdsctl.exe, and opus.dll: $Resolved"
    }
    return $Resolved
  }

  $Candidates = @(
    (Join-Path $RepoRoot "build\Release"),
    (Join-Path $RepoRoot "build")
  )
  foreach ($Candidate in $Candidates) {
    if (Test-VdsToolsDir -Path $Candidate) {
      return Resolve-VdsPath -Path $Candidate
    }
  }

  throw "tools directory was not found. Build vdsd.exe/vdsctl.exe first or pass -ToolsDir."
}

function ConvertTo-RtfEscapedText {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.Append("{\rtf1\ansi\deff0{\fonttbl{\f0 Consolas;}}\fs18 ")

  foreach ($Char in $Text.ToCharArray()) {
    switch ($Char) {
      "\" {
        [void]$Builder.Append("\\")
      }
      "{" {
        [void]$Builder.Append("\{")
      }
      "}" {
        [void]$Builder.Append("\}")
      }
      "`r" {
      }
      "`n" {
        [void]$Builder.Append("\par`r`n")
      }
      default {
        $CodePoint = [int][char]$Char
        if ($CodePoint -ge 32 -and $CodePoint -le 126) {
          [void]$Builder.Append($Char)
        } elseif ($CodePoint -lt 32) {
        } else {
          [void]$Builder.Append("\u$CodePoint?")
        }
      }
    }
  }

  [void]$Builder.Append("}")
  return $Builder.ToString()
}

function New-LicenseRtf {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $LicensePath = Join-Path $RepoRoot "LICENSE"
  $LicenseText = [System.IO.File]::ReadAllText($LicensePath)
  $LicenseRtf = ConvertTo-RtfEscapedText -Text $LicenseText
  [System.IO.File]::WriteAllText(
    $OutputPath,
    $LicenseRtf,
    [System.Text.Encoding]::ASCII)
}

function ConvertTo-CxxByteArray {
  param(
    [Parameter(Mandatory = $true)]
    [byte[]]$Bytes
  )

  $Lines = New-Object System.Collections.Generic.List[string]
  for ($Offset = 0; $Offset -lt $Bytes.Length; $Offset += 12) {
    $End = [Math]::Min($Offset + 11, $Bytes.Length - 1)
    $Values = for ($Index = $Offset; $Index -le $End; ++$Index) {
      "0x{0:x2}" -f $Bytes[$Index]
    }
    $Lines.Add("  " + ($Values -join ", ") + ",")
  }
  return $Lines -join "`r`n"
}

function New-UninstallPayloadHeader {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $Payloads = @(
    [pscustomobject]@{
      Symbol = "kPayloadStopServicePs1"
      RelativePath = "stop_service.ps1"
      SourcePath = Join-Path $RepoRoot "stop_service.ps1"
    },
    [pscustomobject]@{
      Symbol = "kPayloadUninstallServicePs1"
      RelativePath = "uninstall_service.ps1"
      SourcePath = Join-Path $RepoRoot "uninstall_service.ps1"
    },
    [pscustomobject]@{
      Symbol = "kPayloadUninstallEnvPs1"
      RelativePath = "uninstall_env.ps1"
      SourcePath = Join-Path $RepoRoot "uninstall_env.ps1"
    }
  )

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.AppendLine("#pragma once")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("#include <cstddef>")
  [void]$Builder.AppendLine("#include <cstdint>")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("struct VdsUninstallPayload {")
  [void]$Builder.AppendLine("  const wchar_t *relative_path;")
  [void]$Builder.AppendLine("  const std::uint8_t *data;")
  [void]$Builder.AppendLine("  std::size_t size;")
  [void]$Builder.AppendLine("};")
  [void]$Builder.AppendLine()

  foreach ($Payload in $Payloads) {
    $Bytes = [System.IO.File]::ReadAllBytes($Payload.SourcePath)
    [void]$Builder.AppendLine("inline constexpr std::uint8_t $($Payload.Symbol)[] = {")
    [void]$Builder.AppendLine((ConvertTo-CxxByteArray -Bytes $Bytes))
    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine()
  }

  [void]$Builder.AppendLine("inline constexpr VdsUninstallPayload kVdsUninstallPayloads[] = {")
  foreach ($Payload in $Payloads) {
    [void]$Builder.AppendLine(
      "  {L`"$($Payload.RelativePath)`", $($Payload.Symbol), sizeof($($Payload.Symbol))},"
    )
  }
  [void]$Builder.AppendLine("};")

  [System.IO.File]::WriteAllText(
    $OutputPath,
    $Builder.ToString(),
    [System.Text.Encoding]::ASCII)
}

function New-SetupPayloadHeader {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$MainMsiPath,
    [Parameter(Mandatory = $true)]
    [string]$DisplayVersion
  )

  $Payloads = @(
    [pscustomobject]@{
      Symbol = "kPayloadMainMsi"
      FileName = "vDS-setup.msi"
      SourcePath = $MainMsiPath
    },
    [pscustomobject]@{
      Symbol = "kPayloadInstallDependenciesPs1"
      FileName = "install_dependencies.ps1"
      SourcePath = Join-Path $ScriptDir "install_dependencies.ps1"
    }
  )

  $Builder = [System.Text.StringBuilder]::new()
  [void]$Builder.AppendLine("#pragma once")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("#include <cstddef>")
  [void]$Builder.AppendLine("#include <cstdint>")
  [void]$Builder.AppendLine()
  $EscapedDisplayVersion = $DisplayVersion.Replace("\", "\\").Replace('"', '\"')
  [void]$Builder.AppendLine("inline constexpr const wchar_t *kVdsSetupVersion = L`"$EscapedDisplayVersion`";")
  [void]$Builder.AppendLine()
  [void]$Builder.AppendLine("struct VdsSetupPayload {")
  [void]$Builder.AppendLine("  const wchar_t *file_name;")
  [void]$Builder.AppendLine("  const std::uint8_t *data;")
  [void]$Builder.AppendLine("  std::size_t size;")
  [void]$Builder.AppendLine("};")
  [void]$Builder.AppendLine()

  foreach ($Payload in $Payloads) {
    $Bytes = [System.IO.File]::ReadAllBytes($Payload.SourcePath)
    [void]$Builder.AppendLine("inline constexpr std::uint8_t $($Payload.Symbol)[] = {")
    [void]$Builder.AppendLine((ConvertTo-CxxByteArray -Bytes $Bytes))
    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine()
  }

  [void]$Builder.AppendLine("inline constexpr VdsSetupPayload kVdsSetupPayloads[] = {")
  foreach ($Payload in $Payloads) {
    [void]$Builder.AppendLine(
      "  {L`"$($Payload.FileName)`", $($Payload.Symbol), sizeof($($Payload.Symbol))},"
    )
  }
  [void]$Builder.AppendLine("};")

  [System.IO.File]::WriteAllText(
    $OutputPath,
    $Builder.ToString(),
    [System.Text.Encoding]::ASCII)
}

function New-SetupLauncherManifest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $ManifestLines = @(
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
    '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">',
    '  <assemblyIdentity version="1.0.0.0" processorArchitecture="*" name="vDS.Setup" type="win32"/>',
    '  <description>vDS Setup</description>',
    '  <dependency>',
    '    <dependentAssembly>',
    '      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>',
    '    </dependentAssembly>',
    '  </dependency>',
    '  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">',
    '    <security>',
    '      <requestedPrivileges>',
    '        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>',
    '      </requestedPrivileges>',
    '    </security>',
    '  </trustInfo>',
    '  <application xmlns="urn:schemas-microsoft-com:asm.v3">',
    '    <windowsSettings>',
    '      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>',
    '      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>',
    '    </windowsSettings>',
    '  </application>',
    '</assembly>'
  )

  [System.IO.File]::WriteAllText(
    $OutputPath,
    (($ManifestLines -join "`r`n") + "`r`n"),
    [System.Text.Encoding]::UTF8)
}

function Invoke-NativeCompiler {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,
    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  $Compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
  if ($Compiler) {
    & $Compiler.Source @Arguments | Out-Host
  } else {
    $VsDevCmd = Resolve-VsDevCmd
    $VsArch = Resolve-VsArch
    $QuotedArgs = ($Arguments | ForEach-Object { "`"$_`"" }) -join " "
    $Command = "`"$VsDevCmd`" -arch=$VsArch -host_arch=x64 >nul && cl.exe $QuotedArgs"
    & cmd.exe /d /s /c $Command | Out-Host
  }

  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage with exit code $LASTEXITCODE"
  }
}

function Resolve-VsDevCmd {
  $VsWhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
  if ($VsWhereCommand) {
    $VsWhere = $VsWhereCommand.Source
  } else {
    $VsWhereCandidates = @(
      (Join-Path "${env:ProgramFiles(x86)}" "Microsoft Visual Studio\Installer\vswhere.exe"),
      (Join-Path "$env:ProgramFiles" "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    $VsWhere = $VsWhereCandidates |
      Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
      Select-Object -First 1
  }
  if ([string]::IsNullOrWhiteSpace($VsWhere)) {
    throw "vswhere.exe was not found"
  }

  $InstallPath = & $VsWhere `
    -latest `
    -products "*" `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  if ($LASTEXITCODE -eq 0 -and ![string]::IsNullOrWhiteSpace($InstallPath)) {
    $VsDevCmd = Join-Path $InstallPath "Common7\Tools\VsDevCmd.bat"
    if (Test-Path -LiteralPath $VsDevCmd -PathType Leaf) {
      return $VsDevCmd
    }
  }

  throw "VsDevCmd.bat was not found. Install Visual Studio or Build Tools with the C++ toolchain."
}

function Resolve-VsArch {
  switch ($Arch) {
    "x64" {
      return "x64"
    }
    default {
      return $Arch
    }
  }
}

function Build-NativeLauncher {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$IncludeDir = "",
    [string]$ManifestPath = ""
  )

  $Args = @(
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/W4",
    "/DUNICODE",
    "/D_UNICODE",
    "/DWINVER=0x0A00",
    "/D_WIN32_WINNT=0x0A00"
  )
  if (![string]::IsNullOrWhiteSpace($IncludeDir)) {
    $Args += "/I$IncludeDir"
  }

  $Args += @(
    "/Fe:$OutputPath",
    $SourcePath,
    "/link",
    "Advapi32.lib",
    "Shell32.lib",
    "User32.lib"
  )
  if (![string]::IsNullOrWhiteSpace($ManifestPath)) {
    $Args += @(
      "/MANIFEST:EMBED",
      "/MANIFESTINPUT:$ManifestPath"
    )
  }

  Invoke-NativeCompiler -Arguments $Args -FailureMessage "native launcher build failed"
}

function Build-NativeDll {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
  )

  $Args = @(
    "/nologo",
    "/EHsc",
    "/std:c++17",
    "/W4",
    "/DUNICODE",
    "/D_UNICODE",
    "/LD",
    "/Fe:$OutputPath",
    $SourcePath,
    "/link",
    "Msi.lib",
    "Advapi32.lib",
    "User32.lib"
  )

  Invoke-NativeCompiler -Arguments $Args -FailureMessage "native DLL build failed"
}

function Invoke-WixBuild {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,
    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  & $ResolvedWix build @Arguments | Out-Host
  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage with exit code $LASTEXITCODE"
  }
}

$ResolvedOutputDir = Resolve-VdsOutputDir
$ResolvedVersion = Resolve-VdsMsiVersion
$ResolvedDisplayVersion = Get-VdsGitVersionFromGit -RepoRoot $RepoRoot
if ([string]::IsNullOrWhiteSpace($ResolvedDisplayVersion)) {
  $ResolvedDisplayVersion = "unknown"
}
$ResolvedWix = Resolve-Wix
$ResolvedToolsDir = Resolve-VdsToolsDir

$PackageFileNameArgs = @{
  Name = "vDSSetup"
  Arch = $Arch
}
if (![string]::IsNullOrWhiteSpace($PkgRel)) {
  $PackageFileNameArgs.PkgRel = $PkgRel
}
$SetupExeFileName = Resolve-VdsPackageFileName @PackageFileNameArgs
$SetupPath = Join-Path $ResolvedOutputDir $SetupExeFileName

$UninstallRunnerSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\uninstall_runner.cc")
$SetupActionsSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\setup_actions.cc")
$SetupLauncherSource = Resolve-VdsPath -Path (Join-Path $ScriptDir "launcher\setup_launcher.cc")

$LicenseRtf = Join-Path $GeneratedDir "license.rtf"
$UninstallPayloadHeader = Join-Path $GeneratedDir "uninstall_payload.hh"
$SetupPayloadHeader = Join-Path $GeneratedDir "setup_payload.hh"
$SetupLauncherManifest = Join-Path $GeneratedDir "setup-launcher.manifest"
$UninstallRunnerPath = Join-Path $GeneratedDir "vds-uninstall-runner.exe"
$SetupActionsPath = Join-Path $GeneratedDir "vds-setup-actions.dll"
$MainMsiPath = Join-Path $GeneratedDir "vDS-setup.msi"

New-Item -ItemType Directory -Force -Path $GeneratedDir | Out-Null
New-Item -ItemType Directory -Force -Path $ResolvedOutputDir | Out-Null
New-LicenseRtf -OutputPath $LicenseRtf
New-UninstallPayloadHeader -OutputPath $UninstallPayloadHeader

Build-NativeLauncher `
  -SourcePath $UninstallRunnerSource `
  -OutputPath $UninstallRunnerPath `
  -IncludeDir $GeneratedDir
Build-NativeDll `
  -SourcePath $SetupActionsSource `
  -OutputPath $SetupActionsPath

Invoke-WixBuild `
  -Arguments @(
  (Join-Path $WixDir "Product.wxs"),
  "-arch", $Arch,
  "-d", "VdsVersion=$ResolvedVersion",
  "-d", "VdsDisplayVersion=$ResolvedDisplayVersion",
  "-d", "LicenseRtf=$LicenseRtf",
  "-d", "ToolsDir=$ResolvedToolsDir",
  "-d", "SetupActions=$SetupActionsPath",
  "-d", "UninstallRunner=$UninstallRunnerPath",
  "-out", $MainMsiPath
) `
  -FailureMessage "wix main MSI build failed"

New-SetupPayloadHeader `
  -OutputPath $SetupPayloadHeader `
  -MainMsiPath $MainMsiPath `
  -DisplayVersion $ResolvedDisplayVersion
New-SetupLauncherManifest -OutputPath $SetupLauncherManifest
Build-NativeLauncher `
  -SourcePath $SetupLauncherSource `
  -OutputPath $SetupPath `
  -IncludeDir $GeneratedDir `
  -ManifestPath $SetupLauncherManifest

Write-Output $SetupPath
