<#
.SYNOPSIS
Build the pinned upstream Win32Bridge DLL, or explicitly compile the full shell.
.DESCRIPTION
Default output: artifacts/upstream-bridge/{Configuration}/. This is a runtime
DLL and metadata, not an installable app; no guest executables are packaged.
-FullShell requires the external patched Gecko checkout/object tree/compiler.
The full-shell option compiles upstream sources without producing or signing
an AppX. Upstream package identity, assets and guest-content cleanup remain
separate integration work. This script never changes upstream source files.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$WindowsSdkVersion,
    [switch]$FullShell
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$upstream = Join-Path $repoRoot 'external\DesktopMode_uwp'
if (-not (Test-Path (Join-Path $upstream 'Win32Bridge\Win32Bridge.vcxproj'))) {
    throw 'Pinned upstream source is missing. Run: git submodule update --init --recursive'
}
$toolchain = & (Join-Path $PSScriptRoot 'build-uwp.ps1') -CheckOnly -WindowsSdkVersion $WindowsSdkVersion
$sdk = $toolchain.WindowsSdkVersion
$outputKind = if ($FullShell) { 'upstream-shell-compile' } else { 'upstream-bridge' }
$outputDir = Join-Path $repoRoot "artifacts\$outputKind\$Configuration"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

if ($FullShell) {
    foreach ($variable in @('GECKO_W10M_SOURCE', 'GECKO_W10M_OBJ', 'GECKO_W10M_CLANG_CL')) {
        if (-not [Environment]::GetEnvironmentVariable($variable)) {
            throw "Full shell build requires $variable. See external/DesktopMode_uwp/gecko-source/GeckoShellHost/README.md. Gecko is not supplied by this repository."
        }
    }
    foreach ($requiredPath in @(
        (Join-Path $env:GECKO_W10M_SOURCE 'toolkit\components\startup'),
        (Join-Path $env:GECKO_W10M_OBJ 'dist\include\mozilla-config.h'),
        (Join-Path $env:GECKO_W10M_OBJ 'dist\bin\xul.dll'),
        (Join-Path $env:GECKO_W10M_OBJ 'dist\bin\application.ini'),
        $env:GECKO_W10M_CLANG_CL)) {
        if (-not (Test-Path $requiredPath)) { throw "Missing full-shell prerequisite: $requiredPath" }
    }
    $project = Join-Path $upstream 'factoryos-10x-shell\factoryos-10x-shell.csproj'
    Write-Host 'Compiling the full upstream shell; no AppX package or signing will be performed.'
} else {
    $project = Join-Path $upstream 'Win32Bridge\Win32Bridge.vcxproj'
    Write-Host 'Building the upstream Win32Bridge DLL only; no guest programs or shell package.'
}
$arguments = @(
    $project, '/t:Build', '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64',
    "/p:WindowsTargetPlatformVersion=$sdk", "/p:TargetPlatformVersion=$sdk",
    "/p:OutDir=$outputDir\", '/p:AppxPackage=false',
    '/p:GenerateAppxPackageOnBuild=false', '/p:AppxPackageSigningEnabled=false',
    '/p:PackageCertificateThumbprint=', '/p:PackageCertificateKeyFile=',
    "/bl:$(Join-Path $outputDir 'build.binlog')",
    "/flp:logfile=$(Join-Path $outputDir 'build.log');verbosity=normal"
)
if ($FullShell) { $arguments += '/restore' }
else { $arguments += "/p:IntDir=$outputDir\obj\" }
& $toolchain.MSBuild @arguments
if ($LASTEXITCODE -ne 0) { throw "Upstream build failed (MSBuild exit $LASTEXITCODE). See $outputDir." }
if (-not $FullShell -and @(Get-ChildItem $outputDir -Recurse -File -Filter 'Win32Bridge.Runtime.dll').Count -eq 0) {
    throw "The upstream build produced no Win32Bridge.Runtime.dll in $outputDir. Inspect the MSBuild log."
}
Write-Host "Build outputs: $outputDir"
Write-Host 'This compilation does not establish physical Xbox compatibility.'
