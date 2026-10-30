$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script as administrator.' }
Start-Transcript -Path (Join-Path $PSScriptRoot 'finish.log') -Force
$metadata = Get-Content (Join-Path $PSScriptRoot 'session.json') -Raw | ConvertFrom-Json
$installed = 'C:\Program Files\vDS\vdsd.exe'
$backup = 'C:\Program Files\vDS\vdsd.pre-haptic-diag.exe'
if ((Get-FileHash -LiteralPath $backup).Hash -ne $metadata.OriginalHash) { throw 'Original backup hash mismatch; refusing to restore.' }
if ((Get-FileHash -LiteralPath $installed).Hash -ne $metadata.DiagnosticHash) { throw 'Installed daemon changed since test setup; refusing to overwrite it.' }
Stop-Service vdsd
(Get-Service vdsd).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
try {
    $lines = Get-Content -LiteralPath $metadata.Log
    $marker = -1
    for ($i=0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 'HAPTIC_DIAG_BASELINE_V1') { $marker=$i } }
    if ($marker -ge 0) { $lines[$marker..($lines.Count-1)] | Set-Content (Join-Path $PSScriptRoot 'capture.log') -Encoding UTF8 }
} finally {
    Copy-Item -LiteralPath $backup -Destination $installed -Force
    if ((Get-FileHash -LiteralPath $installed).Hash -ne $metadata.OriginalHash) { throw 'Restored daemon hash mismatch.' }
    if ($metadata.OriginalServiceStatus -eq 'Running') { Start-Service vdsd }
    Get-Service vdsd | Format-Table Name,Status
    Stop-Transcript
}
