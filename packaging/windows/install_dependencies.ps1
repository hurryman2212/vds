# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("Check", "Download", "Install")]
  [string]$Mode,
  [Parameter(Mandatory = $true)]
  [string]$DownloadDir,
  [Parameter(Mandatory = $true)]
  [string]$LogPath,
  [string]$ProgressPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

[Net.ServicePointManager]::SecurityProtocol =
  [Net.ServicePointManager]::SecurityProtocol -bor
  [Net.SecurityProtocolType]::Tls12

function Write-VdsDependencyLog {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  $Sanitized = $Message -replace "[`r`n`t]", " "
  $Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
  Add-Content `
    -LiteralPath $LogPath `
    -Value "$Timestamp $Sanitized" `
    -Encoding Unicode
}

function Write-VdsDependencyProgress {
  param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(-1, 100)]
    [int]$PercentComplete,
    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  if ([string]::IsNullOrWhiteSpace($ProgressPath)) {
    return
  }

  $Sanitized = $Message -replace "[`r`n`t]", " "
  [System.IO.File]::WriteAllText(
    $ProgressPath,
    "$PercentComplete`t$Sanitized",
    [System.Text.Encoding]::ASCII)
}

function Test-UsbipInstalled {
  if (Test-Path `
      -LiteralPath (
        "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\" +
        "CurrentVersion\Uninstall\" +
        "{199505b0-b93d-4521-a8c7-897818e0205a}_is1")) {
    return $true
  }

  $ProgramFiles64 = [Environment]::GetEnvironmentVariable("ProgramW6432")
  if ([string]::IsNullOrWhiteSpace($ProgramFiles64)) {
    $ProgramFiles64 = [Environment]::GetFolderPath(
      [Environment+SpecialFolder]::ProgramFiles)
  }

  return Test-Path `
    -LiteralPath (Join-Path $ProgramFiles64 "USBip\usbip.exe") `
    -PathType Leaf
}

function Test-HidHideInstalled {
  if (Test-Path `
      -LiteralPath "Registry::HKEY_CLASSES_ROOT\Installer\Dependencies\NSS.Drivers.HidHide.x64") {
    return $true
  }

  $ProgramFiles64 = [Environment]::GetEnvironmentVariable("ProgramW6432")
  if ([string]::IsNullOrWhiteSpace($ProgramFiles64)) {
    $ProgramFiles64 = [Environment]::GetFolderPath(
      [Environment+SpecialFolder]::ProgramFiles)
  }

  return Test-Path `
    -LiteralPath (
      Join-Path $ProgramFiles64 `
        "Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe") `
    -PathType Leaf
}

function Get-VdsLatestReleaseAsset {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Repository,
    [Parameter(Mandatory = $true)]
    [string]$AssetPattern
  )

  $ApiUri = "https://api.github.com/repos/$Repository/releases/latest"
  Write-VdsDependencyLog "query latest stable release: $ApiUri"
  $Headers = @{
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent" = "vDS-Setup"
  }
  $Release = Invoke-RestMethod -Uri $ApiUri -Headers $Headers
  if ($Release.draft -or $Release.prerelease) {
    throw "GitHub returned a non-stable latest release for $Repository"
  }

  $Assets = @(
    $Release.assets |
      Where-Object { $_.name -match $AssetPattern }
  )
  if ($Assets.Count -ne 1) {
    throw (
      "expected exactly one x64 installer in the latest $Repository release; " +
      "found $($Assets.Count)")
  }

  $Asset = $Assets[0]
  $ExpectedPrefix = "https://github.com/$Repository/releases/download/"
  if (!$Asset.browser_download_url.StartsWith(
      $ExpectedPrefix,
      [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing an unexpected release URL for $Repository"
  }

  Write-VdsDependencyLog (
    "resolved $Repository $($Release.tag_name): $($Asset.name)")
  return $Asset
}

function Test-VdsReleaseDigest {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Asset,
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $DigestProperty = $Asset.PSObject.Properties["digest"]
  if (!$DigestProperty -or [string]::IsNullOrWhiteSpace($DigestProperty.Value)) {
    Write-VdsDependencyLog (
      "GitHub did not publish a digest for $($Asset.name); " +
      "using Authenticode verification")
    return
  }

  if ($DigestProperty.Value -notmatch "^sha256:([0-9a-fA-F]{64})$") {
    throw "unsupported GitHub release digest for $($Asset.name)"
  }

  $Expected = $Matches[1]
  $Actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
  if ($Actual -ne $Expected) {
    throw "SHA-256 mismatch for $($Asset.name)"
  }
  Write-VdsDependencyLog "verified GitHub SHA-256 for $($Asset.name)"
}

function Test-VdsAuthenticodeSignature {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $Signature = Get-AuthenticodeSignature -LiteralPath $Path
  if ($Signature.Status -ne
      [System.Management.Automation.SignatureStatus]::Valid) {
    throw (
      "invalid Authenticode signature for $(Split-Path -Leaf $Path): " +
      "$($Signature.Status) ($($Signature.StatusMessage))")
  }
  if (!$Signature.SignerCertificate) {
    throw "missing signer certificate for $(Split-Path -Leaf $Path)"
  }

  Write-VdsDependencyLog (
    "verified Authenticode signer for $(Split-Path -Leaf $Path): " +
    $Signature.SignerCertificate.Subject)
}

function Save-VdsDependencyInstaller {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Dependency,
    [Parameter(Mandatory = $true)]
    [int]$DependencyIndex,
    [Parameter(Mandatory = $true)]
    [int]$DependencyCount
  )

  $ProgressStart = [Math]::Floor(
    $DependencyIndex * 100 / $DependencyCount)
  $ProgressEnd = [Math]::Floor(
    ($DependencyIndex + 1) * 100 / $DependencyCount)
  Write-VdsDependencyProgress `
    -PercentComplete $ProgressStart `
    -Message "Resolving the latest $($Dependency.Name) release..."

  $Asset = Get-VdsLatestReleaseAsset `
    -Repository $Dependency.Repository `
    -AssetPattern $Dependency.AssetPattern
  $Destination = Join-Path $DownloadDir $Dependency.FileName
  Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue

  Write-VdsDependencyLog (
    "download $($Asset.browser_download_url) to $Destination")

  $Request = [System.Net.HttpWebRequest]::Create(
    $Asset.browser_download_url)
  $Request.AllowAutoRedirect = $true
  $Request.UserAgent = "vDS-Setup"
  $Response = $null
  $InputStream = $null
  $OutputStream = $null
  try {
    $Response = $Request.GetResponse()
    if ($Response.ResponseUri.Scheme -ne "https") {
      throw "refusing a non-HTTPS redirect for $($Asset.name)"
    }

    $ContentLength = $Response.ContentLength
    $InputStream = $Response.GetResponseStream()
    $OutputStream = [System.IO.File]::Create($Destination)
    $Buffer = New-Object byte[] (64 * 1024)
    [long]$Downloaded = 0
    $LastPercent = -1
    $LastReportedMiB = -1

    while (($BytesRead = $InputStream.Read(
        $Buffer, 0, $Buffer.Length)) -gt 0) {
      $OutputStream.Write($Buffer, 0, $BytesRead)
      $Downloaded += $BytesRead

      if ($ContentLength -gt 0) {
        $FilePercent = [Math]::Min(
          100,
          [Math]::Floor($Downloaded * 100 / $ContentLength))
        $OverallPercent = [Math]::Min(
          $ProgressEnd,
          $ProgressStart + [Math]::Floor(
            ($ProgressEnd - $ProgressStart) *
            $Downloaded /
            $ContentLength))
        if ($FilePercent -ne $LastPercent) {
          $DownloadedMiB = $Downloaded / 1MB
          $TotalMiB = $ContentLength / 1MB
          Write-VdsDependencyProgress `
            -PercentComplete $OverallPercent `
            -Message (
              "Downloading $($Dependency.Name): " +
              "$FilePercent% " +
              "($($DownloadedMiB.ToString("0.0")) of " +
              "$($TotalMiB.ToString("0.0")) MiB)")
          $LastPercent = $FilePercent
        }
      } else {
        $DownloadedMiB = [Math]::Floor($Downloaded / 1MB)
        if ($DownloadedMiB -ne $LastReportedMiB) {
          Write-VdsDependencyProgress `
            -PercentComplete $ProgressStart `
            -Message (
              "Downloading $($Dependency.Name): " +
              "$DownloadedMiB MiB")
          $LastReportedMiB = $DownloadedMiB
        }
      }
    }
  } finally {
    if ($OutputStream) {
      $OutputStream.Dispose()
    }
    if ($InputStream) {
      $InputStream.Dispose()
    }
    if ($Response) {
      $Response.Dispose()
    }
  }

  Write-VdsDependencyProgress `
    -PercentComplete $ProgressEnd `
    -Message "Verifying $($Dependency.Name)..."

  Test-VdsReleaseDigest -Asset $Asset -Path $Destination
  Test-VdsAuthenticodeSignature -Path $Destination
  Write-VdsDependencyProgress `
    -PercentComplete $ProgressEnd `
    -Message "$($Dependency.Name) is ready."
}

function Install-VdsDependency {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Dependency,
    [Parameter(Mandatory = $true)]
    [int]$DependencyIndex,
    [Parameter(Mandatory = $true)]
    [int]$DependencyCount
  )

  $InstallerPath = Join-Path $DownloadDir $Dependency.FileName
  if (!(Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "downloaded $($Dependency.Name) installer is missing"
  }
  Write-VdsDependencyProgress `
    -PercentComplete -1 `
    -Message "Verifying the $($Dependency.Name) installer..."
  Test-VdsAuthenticodeSignature -Path $InstallerPath

  Write-VdsDependencyLog (
    "install $($Dependency.Name): $InstallerPath " +
    ($Dependency.Arguments -join " "))
  Write-VdsDependencyProgress `
    -PercentComplete -1 `
    -Message "Installing $($Dependency.Name)..."
  $Process = Start-Process `
    -FilePath $InstallerPath `
    -ArgumentList $Dependency.Arguments `
    -Wait `
    -PassThru
  Write-VdsDependencyLog (
    "$($Dependency.Name) installer exit code: $($Process.ExitCode)")

  if ($Process.ExitCode -notin @(0, 3010, 1641)) {
    throw (
      "$($Dependency.Name) installer failed with exit code " +
      $Process.ExitCode)
  }
  $PercentComplete = [Math]::Floor(
    ($DependencyIndex + 1) * 100 / $DependencyCount)
  Write-VdsDependencyProgress `
    -PercentComplete $PercentComplete `
    -Message "$($Dependency.Name) was installed successfully."
  return $Process.ExitCode -in @(3010, 1641)
}

$Dependencies = @(
  [pscustomobject]@{
    Name = "USB/IP"
    Repository = "vadimgrn/usbip-win2"
    AssetPattern = "^USBip-[0-9]+(\.[0-9]+){3}-x64\.exe$"
    FileName = "usbip-win2-installer.exe"
    IsInstalled = { Test-UsbipInstalled }
    Arguments = @(
      "/VERYSILENT",
      "/SUPPRESSMSGBOXES",
      "/NORESTART",
      "/RESTARTEXITCODE=3010",
      "/SP-"
    )
  },
  [pscustomobject]@{
    Name = "HidHide"
    Repository = "nefarius/HidHide"
    AssetPattern = "^HidHide_[0-9]+(\.[0-9]+){2}_x64\.exe$"
    FileName = "hidhide-installer.exe"
    IsInstalled = { Test-HidHideInstalled }
    Arguments = @("/exenoui", "/qn", "REBOOT=ReallySuppress")
  }
)

try {
  if (![Environment]::Is64BitOperatingSystem) {
    throw "the vDS USB/IP and HidHide stack requires 64-bit Windows"
  }
  if ([Environment]::OSVersion.Version -lt [Version]"10.0.18362") {
    throw "USB/IP requires Windows 10 version 1903 or later"
  }

  New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
  $MissingDependencies = @()
  foreach ($Dependency in $Dependencies) {
    $IsInstalled = $Dependency.IsInstalled
    if (& $IsInstalled) {
      Write-VdsDependencyLog (
        "$($Dependency.Name) is already installed; leave it unchanged")
      continue
    }
    $MissingDependencies += $Dependency
  }

  $RestartRequired = $false
  if ($Mode -eq "Check") {
    if ($MissingDependencies.Count -eq 0) {
      exit 0
    }
    Write-VdsDependencyLog (
      "missing dependencies: " +
      (($MissingDependencies | ForEach-Object { $_.Name }) -join ", "))
    exit 10
  } elseif ($Mode -eq "Download") {
    if ($MissingDependencies.Count -eq 0) {
      Write-VdsDependencyProgress `
        -PercentComplete 100 `
        -Message "USB/IP and HidHide are already installed."
    } else {
      for ($Index = 0; $Index -lt $MissingDependencies.Count; ++$Index) {
        Save-VdsDependencyInstaller `
          -Dependency $MissingDependencies[$Index] `
          -DependencyIndex $Index `
          -DependencyCount $MissingDependencies.Count
      }
      Write-VdsDependencyProgress `
        -PercentComplete 100 `
        -Message "USB/IP and HidHide are ready."
    }
  } else {
    if ($MissingDependencies.Count -eq 0) {
      Write-VdsDependencyProgress `
        -PercentComplete 100 `
        -Message "USB/IP and HidHide are already installed."
    } else {
      for ($Index = 0; $Index -lt $MissingDependencies.Count; ++$Index) {
        if (Install-VdsDependency `
            -Dependency $MissingDependencies[$Index] `
            -DependencyIndex $Index `
            -DependencyCount $MissingDependencies.Count) {
          $RestartRequired = $true
        }
      }
      Write-VdsDependencyProgress `
        -PercentComplete 100 `
        -Message "USB/IP and HidHide installation is complete."
    }
  }

  if ($Mode -eq "Install" -and $RestartRequired) {
    Write-VdsDependencyLog "dependency installation requires a restart"
    exit 3010
  }
  exit 0
} catch {
  Write-VdsDependencyLog "dependency $Mode failed: $($_.Exception.Message)"
  Write-Error $_ -ErrorAction Continue
  exit 1
}
