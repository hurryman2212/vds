$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
$compilerBin = [IO.Path]::GetFullPath((Join-Path $PWD '..\vds-tools\llvm-mingw-20261020-ucrt-x86_64\bin'))
New-Item -ItemType Directory -Force diagnostics\bin | Out-Null
$exports = & "$compilerBin\llvm-readobj.exe" --coff-exports 'C:\Program Files\vDS\opus.dll'
$names = $exports | ForEach-Object { if ($_ -match '^\s+Name: (opus_\w+)') { $Matches[1] } }
@('LIBRARY opus.dll','EXPORTS') + $names | Set-Content diagnostics\opus.def -Encoding ascii
& "$compilerBin\llvm-dlltool.exe" -m i386:x86-64 -d diagnostics\opus.def -l diagnostics\libopus.a
if ($LASTEXITCODE) { throw 'Opus import library creation failed' }
@'
#pragma once
namespace vds {
inline constexpr const char *kVersion = "0.4.0-rc1-adaptive-haptics-v3";
inline constexpr const char *kBuildYear = "2026";
}
'@ | Set-Content diagnostics\include\vds_build_info.hh -Encoding ascii
$sources = @('src/vds_protocol.cc','src/vds_common.cc','src/jsonl.cc','src/vds_config.cc','src/vds_log.cc','src/vdsd_common.cc','src/platform/win32/vds_win32.cc','src/platform/win32/vds_io.cc','src/platform/win32/vds_fs.cc','src/platform/win32/vds_bt.cc','src/platform/win32/vds_hidhide.cc','src/platform/win32/vds_usbip.cc','src/platform/win32/vdsd.cc')
$flags = @('-std=c++20','-O2','-static','-DNOMINMAX','-D_WIN32_WINNT=0x0A00','-Iinclude','-Isrc','-Idiagnostics/include')
Write-Output ('clang++ ' + (($flags + $sources) -join ' ') + ' diagnostics/libopus.a -lhid -lsetupapi -lbthprops -lcfgmgr32 -lws2_32 -lwinmm -ladvapi32 -o diagnostics/bin/vdsd.exe')
& "$compilerBin\clang++.exe" @flags @sources diagnostics/libopus.a -lhid -lsetupapi -lbthprops -lcfgmgr32 -lws2_32 -lwinmm -ladvapi32 -o diagnostics/bin/vdsd.exe
if ($LASTEXITCODE) { throw 'Diagnostic daemon build failed' }
Copy-Item 'C:\Program Files\vDS\opus.dll' diagnostics\bin\opus.dll -Force
$ctlSources = @('src/platform/win32/vdsctl.cc','src/vdsctl_common.cc','src/vds_protocol.cc','src/jsonl.cc','src/vds_common.cc','src/vds_config.cc','src/platform/win32/vds_fs.cc','src/platform/win32/vds_win32.cc')
& "$compilerBin\clang++.exe" @flags @ctlSources diagnostics/libopus.a -ladvapi32 -o diagnostics/bin/vdsctl.exe
if ($LASTEXITCODE) { throw 'Controller CLI build failed' }
& "$compilerBin\clang++.exe" @flags diagnostics/metrics_test.cc src/vds_protocol.cc diagnostics/libopus.a -o diagnostics/bin/metrics_test.exe
if ($LASTEXITCODE) { throw 'Diagnostic test build failed' }
& .\diagnostics\bin\metrics_test.exe
if ($LASTEXITCODE) { throw 'Diagnostic tests failed' }
$testSources = $sources | Where-Object { $_ -ne 'src/platform/win32/vdsd.cc' }
& "$compilerBin\clang++.exe" @flags diagnostics/scheduler_test.cc @testSources diagnostics/libopus.a -lhid -lsetupapi -lbthprops -lcfgmgr32 -lws2_32 -lwinmm -ladvapi32 -o diagnostics/bin/scheduler_test.exe
if ($LASTEXITCODE) { throw 'Scheduler test build failed' }
& .\diagnostics\bin\scheduler_test.exe
if ($LASTEXITCODE) { throw 'Scheduler tests failed' }
} finally { Pop-Location }
