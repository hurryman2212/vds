param([ValidateSet('Install','Restore')][string]$Mode = 'Install')
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator.' }
Start-Transcript -Path (Join-Path $PSScriptRoot "fixes-v2-$Mode.log") -Force
try {
    $installed = 'C:\Program Files\vDS\vdsd.exe'
    $backup = 'C:\Program Files\vDS\vdsd.pre-haptic-fixes-v2.exe'
    $manifest = Join-Path $PSScriptRoot 'fixes-v2-session.json'
    if ($Mode -eq 'Install') {
        if ((Test-Path -LiteralPath $backup) -or (Test-Path -LiteralPath $manifest)) { throw 'A v2 backup/session already exists; inspect it before replacing anything.' }
        $replacement = Join-Path $PSScriptRoot 'bin\vdsd.exe'
        $metadata = [ordered]@{
            StartedAt=[DateTimeOffset]::Now.ToString('o')
            OriginalHash=(Get-FileHash -LiteralPath $installed).Hash
            ReplacementHash=(Get-FileHash -LiteralPath $replacement).Hash
            OriginalServiceStatus=(Get-Service vdsd).Status.ToString()
        }
        Stop-Service vdsd
        (Get-Service vdsd).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
        Copy-Item -LiteralPath $installed -Destination $backup
        if ((Get-FileHash -LiteralPath $backup).Hash -ne $metadata.OriginalHash) { throw 'Backup hash mismatch.' }
        $metadata | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
        try {
            Copy-Item -LiteralPath $replacement -Destination $installed -Force
            if ((Get-FileHash -LiteralPath $installed).Hash -ne $metadata.ReplacementHash) { throw 'Replacement hash mismatch.' }
            Start-Service vdsd
            (Get-Service vdsd).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))
        } catch {
            Stop-Service vdsd -ErrorAction SilentlyContinue
            (Get-Service vdsd).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
            Copy-Item -LiteralPath $backup -Destination $installed -Force
            if ($metadata.OriginalServiceStatus -eq 'Running') { Start-Service vdsd }
            throw
        }
        & 'C:\Program Files\vDS\vdsctl.exe' list-targets
        & 'C:\Program Files\vDS\vdsctl.exe' list
    } else {
        $metadata = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        if ((Get-FileHash -LiteralPath $backup).Hash -ne $metadata.OriginalHash) { throw 'Backup hash mismatch.' }
        if ((Get-FileHash -LiteralPath $installed).Hash -ne $metadata.ReplacementHash) { throw 'Installed daemon changed; refusing to overwrite it.' }
        Stop-Service vdsd
        (Get-Service vdsd).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
        Copy-Item -LiteralPath $backup -Destination $installed -Force
        if ((Get-FileHash -LiteralPath $installed).Hash -ne $metadata.OriginalHash) { throw 'Restored hash mismatch.' }
        if ($metadata.OriginalServiceStatus -eq 'Running') { Start-Service vdsd }
    }
    Get-Service vdsd | Format-Table Name,Status
    Get-FileHash -LiteralPath $installed
} finally { Stop-Transcript }
