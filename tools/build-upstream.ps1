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
    [switch]$FullShell,
    [switch]$WithX86Backend
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
$outputKind = if ($FullShell) { 'upstream-shell-compile' } elseif ($WithX86Backend) { 'upstream-bridge-x86' } else { 'upstream-bridge' }
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
    if ($WithX86Backend) {
        # Apply our overlay to a disposable source copy, never the pinned
        # submodule. Keep existing public RuntimeSession/RuntimeHost ABI.
        $relativeSource = 'artifacts/upstream-bridge-x86/source'
        $sourceRoot = Join-Path $repoRoot $relativeSource
        if (Test-Path $sourceRoot) { Remove-Item $sourceRoot -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
        Copy-Item (Join-Path $upstream 'Win32Bridge') $sourceRoot -Recurse
        $patch = Join-Path $repoRoot 'patches\Win32Bridge-x86-startup.patch'
        & git -C $repoRoot apply --ignore-space-change "--directory=$relativeSource" --check $patch
        if ($LASTEXITCODE -ne 0) { throw 'The x86 bridge overlay does not apply to the pinned source.' }
        & git -C $repoRoot apply --ignore-space-change "--directory=$relativeSource" $patch
        if ($LASTEXITCODE -ne 0) { throw 'Applying the x86 bridge overlay failed.' }
        $ownershipPatch = Join-Path $repoRoot 'patches\Win32Bridge-wine-ownership.patch'
        & git -C $repoRoot apply --ignore-space-change "--directory=$relativeSource" --check $ownershipPatch
        if ($LASTEXITCODE -ne 0) { throw 'The Wine ownership overlay does not apply after the x86 overlay.' }
        & git -C $repoRoot apply --ignore-space-change "--directory=$relativeSource" $ownershipPatch
        if ($LASTEXITCODE -ne 0) { throw 'Applying the Wine ownership overlay failed.' }
        $project = Join-Path $sourceRoot 'Win32Bridge\Win32Bridge.vcxproj'
        [xml]$projectXml = Get-Content -Raw $project
        $ns = New-Object System.Xml.XmlNamespaceManager($projectXml.NameTable)
        $ns.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')
        $targetImport = $projectXml.SelectSingleNode("/m:Project/m:Import[contains(@Project, 'Microsoft.Cpp.targets')]", $ns)
        if (-not $targetImport) { throw 'The upstream project has no C++ targets import.' }
        $overlay = $projectXml.CreateElement('Import', $projectXml.DocumentElement.NamespaceURI)
        $overlay.SetAttribute('Project', (Join-Path $PSScriptRoot 'Win32Bridge-x86.props'))
        $projectXml.DocumentElement.InsertBefore($overlay, $targetImport) | Out-Null
        $projectXml.Save($project)
        Write-Host 'Building the patched bridge with PE32 interpreter startup support.'
    }
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
if ($WithX86Backend -and -not $FullShell) {
    $component = Get-ChildItem $outputDir -Recurse -File -Filter 'Win32Bridge.Runtime.dll' | Select-Object -First 1
    $destination = Join-Path $repoRoot 'app\BridgeRuntime'
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item $component.FullName (Join-Path $destination 'Win32Bridge.Runtime.dll') -Force
    Write-Host 'Copied patched component into the UWP host package staging folder.'
}
Write-Host "Build outputs: $outputDir"
Write-Host 'This compilation does not establish physical Xbox compatibility.'
