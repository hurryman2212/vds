# SPDX-License-Identifier: MIT

$script:VdsScriptRoot = if ($PSScriptRoot) {
  $PSScriptRoot
} else {
  Split-Path -Parent $MyInvocation.MyCommand.Path
}
function Invoke-VdsGit {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments
  )

  $GitArguments = @("-C", $RepoRoot) + $Arguments
  $Output = & git @GitArguments 2>$null
  if ($LASTEXITCODE -ne 0) {
    throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
  }
  return ,@($Output)
}

function Test-VdsWorkTreeDirty {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
  )

  $GitArguments = @("-C", $RepoRoot, "diff-index", "--quiet", "HEAD", "--")
  & git @GitArguments 2>$null
  return $LASTEXITCODE -ne 0
}

function Get-VdsSemverParts {
  param(
    [Parameter(Mandatory = $true)]
    [string]$TagName
  )

  $BaseTagName = $TagName
  $RcPart = $null
  if ($TagName -match "^(\d+\.\d+\.\d+)-rc(\d+)$") {
    $BaseTagName = $Matches[1]
    $RcPart = $Matches[2]
  } elseif ($TagName -notmatch "^\d+\.\d+\.\d+$") {
    return $null
  }

  $Values = @()
  foreach ($Part in ($BaseTagName -split "\.")) {
    [int]$Value = 0
    if (![int]::TryParse($Part, [ref]$Value)) {
      return $null
    }
    $Values += $Value
  }
  while ($Values.Count -lt 4) {
    $Values += 0
  }

  if ($null -ne $RcPart) {
    [int]$RcValue = 0
    if (![int]::TryParse($RcPart, [ref]$RcValue)) {
      return $null
    }
    $Values[3] = $RcValue
  }

  return $Values
}

function Compare-VdsSemverTag {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Left,
    [Parameter(Mandatory = $true)]
    [string]$Right
  )

  $LeftParts = @(Get-VdsSemverParts -TagName $Left)
  $RightParts = @(Get-VdsSemverParts -TagName $Right)
  for ($Index = 0; $Index -lt 4; ++$Index) {
    if ($LeftParts[$Index] -gt $RightParts[$Index]) {
      return 1
    }
    if ($LeftParts[$Index] -lt $RightParts[$Index]) {
      return -1
    }
  }
  return 0
}

function Get-VdsVersionTagInfoFromGit {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
  )

  if (!(Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git.exe is required to generate the driver version"
  }

  try {
    $InsideWorkTree = Invoke-VdsGit -RepoRoot $RepoRoot -Arguments @(
      "rev-parse",
      "--is-inside-work-tree"
    )
  } catch {
    return $null
  }
  if ($InsideWorkTree.Count -eq 0 -or $InsideWorkTree[0] -ne "true") {
    return $null
  }

  $Tags = Invoke-VdsGit -RepoRoot $RepoRoot -Arguments @(
    "tag",
    "--merged",
    "HEAD",
    "--list"
  )

  $BestTag = $null
  $BestDistance = -1
  foreach ($Tag in $Tags) {
    $TagParts = Get-VdsSemverParts -TagName $Tag
    if ($null -eq $TagParts) {
      continue
    }

    $CommitLines = Invoke-VdsGit -RepoRoot $RepoRoot -Arguments @(
      "rev-list",
      "-n",
      "1",
      $Tag
    )
    if ($CommitLines.Count -eq 0) {
      continue
    }

    $DistanceLines = Invoke-VdsGit -RepoRoot $RepoRoot -Arguments @(
      "rev-list",
      "--count",
      "$($CommitLines[0])..HEAD"
    )
    if ($DistanceLines.Count -eq 0) {
      continue
    }

    [int]$Distance = 0
    if (![int]::TryParse($DistanceLines[0], [ref]$Distance)) {
      continue
    }

    $BetterTag = $false
    if ($BestDistance -lt 0 -or $Distance -lt $BestDistance) {
      $BetterTag = $true
    } elseif ($Distance -eq $BestDistance -and
      (Compare-VdsSemverTag -Left $Tag -Right $BestTag) -gt 0) {
      $BetterTag = $true
    }

    if ($BetterTag) {
      $BestTag = $Tag
      $BestDistance = $Distance
    }
  }

  if ($BestTag) {
    return [pscustomobject]@{
      Tag = $BestTag
      Distance = $BestDistance
    }
  }
  return $null
}

function Get-VdsGitVersionFromGit {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
  )

  try {
    $TagInfo = Get-VdsVersionTagInfoFromGit -RepoRoot $RepoRoot
    if (!$TagInfo) {
      return $null
    }

    $DirtySuffix = ""
    if (Test-VdsWorkTreeDirty -RepoRoot $RepoRoot) {
      $DirtySuffix = "+"
    }

    if ($TagInfo.Distance -eq 0) {
      return "$($TagInfo.Tag)$DirtySuffix"
    }

    $ShaLines = Invoke-VdsGit -RepoRoot $RepoRoot -Arguments @(
      "rev-parse",
      "--short=7",
      "HEAD"
    )
    if ($ShaLines.Count -eq 0) {
      return $null
    }

    return "$($TagInfo.Tag)-$($TagInfo.Distance)-g$($ShaLines[0])$DirtySuffix"
  } catch {
    return $null
  }
}

function ConvertTo-VdsWindowsPackageVersion {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Version
  )

  if ([string]::IsNullOrWhiteSpace($Version)) {
    return "unknown"
  }

  $Sanitized = $Version.Trim()
  foreach ($Char in [System.IO.Path]::GetInvalidFileNameChars()) {
    $Sanitized = $Sanitized.Replace($Char, "_")
  }
  if ([string]::IsNullOrWhiteSpace($Sanitized)) {
    return "unknown"
  }
  return $Sanitized
}

function Resolve-VdsPackageFileName {
  param(
    [string]$Name = "",
    [string]$PkgRel = "",
    [string]$Arch = "x64"
  )

  if ([string]::IsNullOrWhiteSpace($Name)) {
    $Name = Split-Path -Leaf $script:VdsScriptRoot
  }

  $Version = Get-VdsGitVersionFromGit -RepoRoot $script:VdsScriptRoot
  if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = "unknown"
  }

  $PkgVer = ConvertTo-VdsWindowsPackageVersion -Version $Version
  $ReleasePart = ""
  if (![string]::IsNullOrWhiteSpace($PkgRel)) {
    $ReleasePart = "-$PkgRel"
  }
  return "$Name-$PkgVer$ReleasePart-$Arch.exe"
}

function Show-VdsVersionUsage {
  Write-Output "Usage:"
  Write-Output "  generate_version.ps1 [-Type Tool]"
  Write-Output "  generate_version.ps1 -Type Package [-Name NAME] [-Arch ARCH] [-PkgRel REL]"
  Write-Output ""
  Write-Output "Common options:"
  Write-Output "  -Type TYPE             Output type: Tool or Package. Defaults to Tool."
  Write-Output "  -h, --help             Show this help text."
  Write-Output ""
  Write-Output "Package options:"
  Write-Output "  -Arch ARCH             Package architecture. Defaults to x64."
  Write-Output "  -Name NAME             Package name. Defaults to this script's directory name."
  Write-Output "  -PkgRel REL            Package release. Omitted from filenames if unset."
}

function Invoke-VdsVersionMain {
  param(
    [string[]]$Arguments
  )

  $Type = "Tool"
  $Name = ""
  $PkgRel = ""
  $Arch = "x64"
  for ($Index = 0; $Index -lt $Arguments.Count; ++$Index) {
    switch ($Arguments[$Index]) {
      "-Type" {
        ++$Index
        if ($Index -ge $Arguments.Count) {
          Write-Error "missing value for -Type"
          exit 2
        }
        $Type = $Arguments[$Index]
        if ($Type -notin @("Tool", "Package")) {
          Write-Error "Type must be Tool or Package: $Type"
          exit 2
        }
      }
      "-Arch" {
        ++$Index
        if ($Index -ge $Arguments.Count) {
          Write-Error "missing value for -Arch"
          exit 2
        }
        $Arch = $Arguments[$Index]
      }
      "-Name" {
        ++$Index
        if ($Index -ge $Arguments.Count) {
          Write-Error "missing value for -Name"
          exit 2
        }
        $Name = $Arguments[$Index]
      }
      "-PkgRel" {
        ++$Index
        if ($Index -ge $Arguments.Count) {
          Write-Error "missing value for -PkgRel"
          exit 2
        }
        $PkgRel = $Arguments[$Index]
      }
      "-h" {
        Show-VdsVersionUsage
        exit 0
      }
      "--help" {
        Show-VdsVersionUsage
        exit 0
      }
      default {
        Write-Error "unknown option: $($Arguments[$Index])"
        Show-VdsVersionUsage
        exit 2
      }
    }
  }

  switch ($Type) {
    "Tool" {
      $Version = Get-VdsGitVersionFromGit -RepoRoot $script:VdsScriptRoot
      if (!$Version) {
        $Version = "unknown"
      }
      Write-Output $Version
      return
    }
    "Package" {
      Write-Output (Resolve-VdsPackageFileName `
        -Name $Name `
        -PkgRel $PkgRel `
        -Arch $Arch)
      return
    }
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  Invoke-VdsVersionMain -Arguments $args
}
