$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script as administrator.' }
Start-Transcript -Path (Join-Path $PSScriptRoot 'setup.log') -Force
$installed = 'C:\Program Files\vDS\vdsd.exe'
$backup = 'C:\Program Files\vDS\vdsd.pre-haptic-diag.exe'
$diagnostic = Join-Path $PSScriptRoot 'bin\vdsd.exe'
if (Test-Path -LiteralPath $backup) { throw 'A diagnostic backup already exists; restore it before starting another test.' }
$originalHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
$diagnosticHash = (Get-FileHash -LiteralPath $diagnostic -Algorithm SHA256).Hash
$metadata = [ordered]@{ StartedAt=[DateTimeOffset]::Now.ToString('o'); OriginalHash=$originalHash; DiagnosticHash=$diagnosticHash; OriginalServiceStatus=(Get-Service vdsd).Status.ToString(); Log='C:\ProgramData\vDS\vdsd.log' }
$metadata | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'session.json') -Encoding UTF8
Stop-Service vdsd -ErrorAction Stop
(Get-Service vdsd).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
Copy-Item -LiteralPath $installed -Destination $backup
if ((Get-FileHash -LiteralPath $backup).Hash -ne $originalHash) { throw 'Backup verification failed.' }
try {
    Copy-Item -LiteralPath $diagnostic -Destination $installed -Force
    if ((Get-FileHash -LiteralPath $installed).Hash -ne $diagnosticHash) { throw 'Installed diagnostic hash mismatch.' }
    Start-Service vdsd
    (Get-Service vdsd).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))
    & 'C:\Program Files\vDS\vdsctl.exe' list-targets
    & 'C:\Program Files\vDS\vdsctl.exe' list
    Get-Service vdsd | Format-Table Name,Status
} catch {
    Stop-Service vdsd -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $backup -Destination $installed -Force
    throw
} finally { Stop-Transcript }
