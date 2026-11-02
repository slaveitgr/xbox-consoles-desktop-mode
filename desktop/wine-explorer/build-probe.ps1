<#
.SYNOPSIS
Build the self-authored x64 Wine Explorer COM/view qualification guest.
.DESCRIPTION
Produces a normal PE32+ diagnostic for the existing Win32Bridge. This does not
build Wine Explorer, create a UWP package, deploy to Xbox, or execute the guest.
#>
[CmdletBinding()]
param([string]$WindowsSdkVersion)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$toolchain = & (Join-Path $PSScriptRoot 'find-desktop-toolchain.ps1') -WindowsSdkVersion $WindowsSdkVersion
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& $toolchain.MSBuild (Join-Path $PSScriptRoot 'ExplorerContractProbe.vcxproj') `
    /t:Build /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
    "/p:WindowsTargetPlatformVersion=$($toolchain.WindowsSdkVersion)" `
    "/bl:$(Join-Path $output 'build.binlog')"
if ($LASTEXITCODE -ne 0) { throw "Explorer contract probe build failed: $LASTEXITCODE" }
$executable = Join-Path $output 'ExplorerContractProbe.exe'
if (-not (Test-Path $executable)) { throw 'No ExplorerContractProbe.exe was produced.' }
Write-Host "Built qualification guest: $executable"
Write-Host 'Wine Explorer has not been built or run by this command.'
