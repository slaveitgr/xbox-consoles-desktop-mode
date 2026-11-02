# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$WindowsSdkVersion)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio vswhere.exe is missing.' }
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($installation)) { throw 'Visual Studio desktop C++ tools are missing.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$dumpbin = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -Last 1
if (-not (Test-Path $msbuild) -or [string]::IsNullOrWhiteSpace($dumpbin) -or -not (Test-Path $dumpbin)) {
    throw 'Desktop MSBuild or dumpbin.exe is missing.'
}
$buildSdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$available = @(Get-ChildItem (Join-Path $buildSdkRoot 'Include') -Directory | Where-Object {
    $_.Name -match '^10\.0\.\d+\.\d+$' -and
    (Test-Path (Join-Path $_.FullName 'um\Windows.h')) -and
    (Test-Path (Join-Path $buildSdkRoot "Lib\$($_.Name)\um\x64\kernel32.lib"))
} | Sort-Object { [version]$_.Name })
if ($WindowsSdkVersion) {
    if (-not ($available | Where-Object Name -eq $WindowsSdkVersion)) { throw "Windows desktop SDK $WindowsSdkVersion is missing." }
    $sdk = $WindowsSdkVersion
} else {
    if ($available.Count -eq 0) { throw 'No usable x64 Windows desktop SDK is installed.' }
    $sdk = $available[-1].Name
}
[pscustomobject]@{ MSBuild = $msbuild; Dumpbin = $dumpbin; VisualStudio = $installation; WindowsSdkVersion = $sdk }
