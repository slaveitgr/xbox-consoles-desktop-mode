# SPDX-License-Identifier: GPL-2.0-only
# Compile/link the existing pinned SDL/WinRT DLL with MSVC C++/CX. No execution.
# Source sparse-checkout requirement: lib/sdl2 at the full revision below.
param(
    [string]$Source = 'external/Boxedwine64-full',
    [string]$SdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$pin = '58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$artifacts = Join-Path $repoRoot 'artifacts/sdl-winrt'
$buildRoot = Join-Path $repoRoot 'build/sdl-winrt'
New-Item -ItemType Directory -Force $artifacts | Out-Null
$proof = [ordered]@{
    status = 'incomplete'
    sourcePin = $pin
    sourceCommit = $env:GITHUB_SHA
    sdk = $SdkVersion
    compiler = 'MSVC v143 with original per-source C++/CX'
    configuration = 'Release|x64'
    kind = 'existing-native-sdl-winrt-dll-compile-link-only'
    libraryExecuted = $false
    appContainerActivated = $false
    coreWindowCreated = $false
    gpuPresentationTested = $false
    wineExecuted = $false
    xboxTested = $false
}
function Save-Proof {
    $proof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $artifacts 'result.json') -Encoding utf8
}
Save-Proof
foreach ($oldFile in @('build.log', 'build-diagnostic.log', 'build.binlog', 'headers.txt',
    'imports.txt', 'exports.txt', 'source-items.txt', 'msbuild-arguments.json',
    'SDL-UWP.effective.vcxproj', 'AppPartitionGuard.cpp', 'SDL_winrtgamebar.effective.cpp')) {
    Remove-Item -LiteralPath (Join-Path $artifacts $oldFile) -ErrorAction SilentlyContinue
}
try {
    $sourceRoot = (Resolve-Path $Source).Path
    $actualPin = (& git -C $sourceRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualPin -ne $pin) { throw "Unexpected SDL source revision: $actualPin" }
    $dirty = & git -C $sourceRoot status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Pinned source must remain pristine, including untracked files.' }
    $sdlOriginal = Join-Path $sourceRoot 'lib/sdl2'
    $relativeProject = 'VisualC-WinRT/UWP_VS2015/SDL-UWP.vcxproj'
    $originalProject = Join-Path $sdlOriginal $relativeProject
    if (-not (Test-Path $originalProject)) { throw 'Fetch the pinned lib/sdl2 sparse subtree, including VisualC-WinRT and src.' }
    $proof['originalProjectSha256'] = (Get-FileHash $originalProject -Algorithm SHA256).Hash.ToLowerInvariant()

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = (& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $vsPath) { throw 'Visual Studio 2022 C++ x64 tools are required.' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -winsdk=$SdkVersion"
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $msbuild = Join-Path $vsPath 'MSBuild/Current/Bin/MSBuild.exe'
    $storeCrt = Join-Path $env:VCToolsInstallDir 'lib/x64/store'
    foreach ($library in @('msvcrt.lib', 'msvcprt.lib', 'vccorlib.lib')) {
        if (-not (Test-Path (Join-Path $storeCrt $library))) {
            throw "Missing genuine Store CRT/C++ CX library $library in $storeCrt. Install the VS C++ UWP/C++ CX component."
        }
    }
    if (-not (Test-Path (Join-Path $env:WindowsSdkDir "Include/$SdkVersion/um/Windows.h"))) {
        throw "Windows SDK $SdkVersion headers are unavailable."
    }
    $env:LIB = $storeCrt + ';' + $env:LIB
    $proof['compilerPath'] = $compiler
    $proof['compilerFileVersion'] = (Get-Item $compiler).VersionInfo.FileVersion
    $proof['msbuildFileVersion'] = (Get-Item $msbuild).VersionInfo.FileVersion
    $proof['storeCrtDirectory'] = $storeCrt
    $proof['windowsSdkDirectory'] = $env:WindowsSdkDir

    # Only remove this task's disposable output. Preserve the exact upstream
    # tree and keep retargeting/proof additions in an independently staged copy.
    if (Test-Path $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    $stagedSdl = Join-Path $buildRoot 'sdl-source'
    $output = Join-Path $buildRoot 'out'
    $objects = Join-Path $buildRoot 'obj'
    $userProps = Join-Path $buildRoot 'empty-user-props'
    foreach ($directory in @($stagedSdl, $output, $objects, $userProps)) {
        New-Item -ItemType Directory -Force $directory | Out-Null
    }
    Get-ChildItem -LiteralPath $sdlOriginal -Force | Copy-Item -Destination $stagedSdl -Recurse -Force

    # Marked build-local compatibility change, retaining SDL's license and all
    # Game Bar activation/subscription/dispatch/removal code. Current SDKs put
    # the EventHandler<IInspectable*> ABI specialization in the owning Gaming
    # header, rather than making it available from Foundation alone. Use that
    # real SDK declaration; do not recreate its type or disable the events.
    $gameBarRelative = 'src/video/winrt/SDL_winrtgamebar.cpp'
    $gameBarOriginal = Join-Path $sdlOriginal $gameBarRelative
    $gameBarStaged = Join-Path $stagedSdl $gameBarRelative
    $gameBarSource = [System.IO.File]::ReadAllText($gameBarStaged)
    $foundationInclude = '#include <windows.foundation.h>'
    if ([regex]::Matches($gameBarSource, '(?m)^#include <windows\.foundation\.h>\r?$').Count -ne 1 -or
        [regex]::Matches($gameBarSource, '\b__FIEventHandler_1_IInspectable\b').Count -ne 4 -or
        $gameBarSource.Contains('#include <windows.gaming.ui.h>')) {
        throw 'Pinned Game Bar include/event inventory changed; inspect before applying the SDK compatibility overlay.'
    }
    $gamingHeader = Join-Path $env:WindowsSdkDir "Include/$SdkVersion/winrt/windows.gaming.ui.h"
    if (-not (Test-Path $gamingHeader) -or
        -not ([System.IO.File]::ReadAllText($gamingHeader).Contains('DEF___FIEventHandler_1_IInspectable_USE'))) {
        throw 'The selected SDK must supply the real Game Bar EventHandler<IInspectable*> ABI specialization.'
    }
    $gameBarNewline = if ($gameBarSource.Contains("`r`n")) { "`r`n" } else { "`n" }
    $gameBarReplacement = $foundationInclude + $gameBarNewline +
        '/* ProtonDesktop staging overlay: use the owning SDK Game Bar event ABI declaration. */' + $gameBarNewline +
        '#include <windows.gaming.ui.h>'
    [System.IO.File]::WriteAllText($gameBarStaged,
        $gameBarSource.Replace($foundationInclude, $gameBarReplacement), [System.Text.UTF8Encoding]::new($false))
    Copy-Item $gameBarStaged (Join-Path $artifacts 'SDL_winrtgamebar.effective.cpp')
    $proof['gameBarSdkIncludeOverlay'] = [ordered]@{
        source = $gameBarRelative
        originalSha256 = (Get-FileHash $gameBarOriginal -Algorithm SHA256).Hash.ToLowerInvariant()
        effectiveSha256 = (Get-FileHash $gameBarStaged -Algorithm SHA256).Hash.ToLowerInvariant()
        sdkHeader = $gamingHeader
        sdkHeaderSha256 = (Get-FileHash $gamingHeader -Algorithm SHA256).Hash.ToLowerInvariant()
        sdkDeclarationSource = 'https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.gaming.ui.h'
        change = 'Include the owning SDK header only; original Game Bar event behavior retained.'
    }
    Save-Proof

    $projectPath = Join-Path $stagedSdl $relativeProject
    $project = [System.Xml.XmlDocument]::new()
    $project.PreserveWhitespace = $true
    $project.Load($projectPath)
    $xmlNamespace = $project.DocumentElement.NamespaceURI
    $namespaces = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
    $namespaces.AddNamespace('m', $xmlNamespace)
    $sourceItems = @($project.SelectNodes('//m:ClCompile[@Include]', $namespaces))
    if ($sourceItems.Count -ne 114) { throw 'Pinned SDL UWP source inventory changed; inspect before updating the build.' }
    $cxItems = @($sourceItems | Where-Object {
        $_.SelectNodes('m:CompileAsWinRT', $namespaces) | Where-Object {
            $_.InnerText -eq 'true' -and $_.GetAttribute('Condition') -match 'Release\|x64'
        }
    })
    if ($cxItems.Count -eq 0) { throw 'Pinned SDL UWP project must preserve its real per-file C++/CX compilation.' }
    $proof['originalSourceItems'] = $sourceItems.Count
    $proof['originalReleaseX64CxxCxItems'] = $cxItems.Count
    $sourceItems | ForEach-Object { $_.GetAttribute('Include') } | Set-Content (Join-Path $artifacts 'source-items.txt')

    # An extra compile-only TU verifies actual SDK partitions, native WinRT
    # video and D3D11 configuration. A real CoreWindow^ type requires /ZW.
    $guard = Join-Path $buildRoot 'AppPartitionGuard.cpp'
    @'
// Self-authored compile guard; no runtime or GPU activation.
#include <winapifamily.h>
#include <sdkddkver.h>
#include "SDL.h"
#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#error The existing SDL WinRT DLL must compile in the APP SDK partition.
#endif
#if !defined(SDL_BUILDING_WINRT) || SDL_BUILDING_WINRT != 1
#error SDL WinRT source restrictions are required.
#endif
#if !SDL_VIDEO_DRIVER_WINRT || !SDL_VIDEO_RENDER_D3D11
#error The real existing WinRT video and D3D11 renderer must be selected.
#endif
#if !defined(_M_X64)
#error This first native SDL WinRT qualification is x64.
#endif
static_assert(sizeof(void*) == 8, "x64 ABI required");
void ProtonSdlWinrtCompileGuard(Windows::UI::Core::CoreWindow^) {}
'@ | Set-Content $guard -Encoding utf8

    function Add-XmlElement($parent, [string]$name, [string]$text) {
        $element = $project.CreateElement($name, $xmlNamespace)
        if ($null -ne $text) { $element.InnerText = $text }
        [void]$parent.AppendChild($element)
        return $element
    }
    $cppTargets = $project.SelectSingleNode('//m:Import[contains(@Project,"Microsoft.Cpp.targets")]', $namespaces)
    if (-not $cppTargets) { throw 'Pinned MSBuild target import is missing.' }
    $definitions = $project.CreateElement('ItemDefinitionGroup', $xmlNamespace)
    $definitions.SetAttribute('Condition', "'`$(Configuration)|`$(Platform)'=='Release|x64'")
    $compile = Add-XmlElement $definitions 'ClCompile' $null
    [void](Add-XmlElement $compile 'PreprocessorDefinitions' 'WINAPI_FAMILY=WINAPI_FAMILY_APP;SDL_BUILDING_WINRT=1;_WIN32_WINNT=0x0A00;WINVER=0x0A00;NTDDI_VERSION=0x0A000000;%(PreprocessorDefinitions)')
    [void](Add-XmlElement $compile 'RuntimeLibrary' 'MultiThreadedDLL')
    $link = Add-XmlElement $definitions 'Link' $null
    [void](Add-XmlElement $link 'AdditionalLibraryDirectories' "$storeCrt;%(AdditionalLibraryDirectories)")
    [void](Add-XmlElement $link 'AdditionalDependencies' 'WindowsApp.lib;d3d11.lib;dxgi.lib;dxguid.lib;%(AdditionalDependencies)')
    [void](Add-XmlElement $link 'AdditionalOptions' '/APPCONTAINER %(AdditionalOptions)')
    [void]$project.DocumentElement.InsertBefore($definitions, $cppTargets)
    $guardItems = $project.CreateElement('ItemGroup', $xmlNamespace)
    $guardItem = Add-XmlElement $guardItems 'ClCompile' $null
    $guardItem.SetAttribute('Include', $guard)
    [void](Add-XmlElement $guardItem 'CompileAsWinRT' 'true')
    [void]$project.DocumentElement.InsertBefore($guardItems, $cppTargets)
    $project.Save($projectPath)
    Copy-Item $projectPath (Join-Path $artifacts 'SDL-UWP.effective.vcxproj')
    Copy-Item $guard (Join-Path $artifacts 'AppPartitionGuard.cpp')

    $arguments = @($projectPath, '/t:Rebuild', '/m:4', '/nologo', '/v:normal',
        '/p:Configuration=Release', '/p:Platform=x64', '/p:PlatformToolset=v143',
        "/p:WindowsTargetPlatformVersion=$SdkVersion", "/p:TargetPlatformVersion=$SdkVersion",
        '/p:WindowsTargetPlatformMinVersion=10.0.17763.0', '/p:TargetPlatformMinVersion=10.0.17763.0',
        '/p:ApplicationTypeRevision=10.0', '/p:AppContainerApplication=true',
        '/p:GenerateAppxPackageOnBuild=false', '/p:AppxPackageSigningEnabled=false',
        "/p:OutDir=$output/", "/p:IntDir=$objects/", "/p:UserRootDir=$userProps/",
        "/bl:$(Join-Path $artifacts 'build.binlog')",
        "/flp:logfile=$(Join-Path $artifacts 'build-diagnostic.log');verbosity=diagnostic")
    $arguments | ConvertTo-Json | Set-Content (Join-Path $artifacts 'msbuild-arguments.json')
    & $msbuild @arguments 2>&1 | Tee-Object (Join-Path $artifacts 'build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Actual existing SDL WinRT MSVC /ZW build failed. Inspect build.log/binlog; no fallback was attempted.' }
    $dll = Join-Path $output 'SDL2.dll'
    $library = Join-Path $output 'SDL2.lib'
    if (-not (Test-Path $dll) -or -not (Test-Path $library)) { throw 'The native DLL/import library was not produced.' }
    foreach ($mode in @('headers', 'imports', 'exports')) {
        & dumpbin "/$mode" $dll | Set-Content (Join-Path $artifacts "$mode.txt")
        if ($LASTEXITCODE -ne 0) { throw "dumpbin /$mode failed." }
    }
    $imports = Get-Content (Join-Path $artifacts 'imports.txt') -Raw
    $dependencies = @([regex]::Matches($imports, '(?im)^\s*([a-z0-9_.-]+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
    $desktopCrt = @($dependencies | Where-Object {
        $_ -match '^(?:vcruntime|msvcp|concrt|vccorlib)[0-9_a-z]*\.dll$' -and $_ -notmatch '_app\.dll$'
    })
    if ($desktopCrt.Count) {
        throw 'Native SDL WinRT DLL linked a desktop C++ runtime.'
    }
    if ($imports -notmatch '(?i)(?:VCRUNTIME|MSVCP|VCCORLIB)[0-9_a-z]*_APP\.dll') {
        throw 'Native SDL WinRT DLL lacks the genuine APP runtime imports.'
    }
    $headers = Get-Content (Join-Path $artifacts 'headers.txt') -Raw
    if ($headers -notmatch '(?i)App Container') { throw 'Native SDL WinRT DLL lacks the APP_CONTAINER PE characteristic.' }
    $exports = Get-Content (Join-Path $artifacts 'exports.txt') -Raw
    foreach ($symbol in @('SDL_WinRTRunApp', 'SDL_CreateWindow', 'SDL_CreateRenderer', 'SDL_RenderPresent')) {
        if ($exports -notmatch "(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+$symbol(?:\s|$)") {
            throw "Missing actual public SDL C API export: $symbol"
        }
    }
    $dirtyAfter = & git -C $sourceRoot status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $dirtyAfter) { throw 'Original pinned checkout was unexpectedly modified.' }
    $proof['status'] = 'compiled-linked-native-sdl-winrt-only'
    $proof['effectiveProjectSha256'] = (Get-FileHash $projectPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $proof['dllSha256'] = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    $proof['importLibrarySha256'] = (Get-FileHash $library -Algorithm SHA256).Hash.ToLowerInvariant()
    $proof['appSdkGuardCompiled'] = $true
    $proof['appCrtImportsObserved'] = $true
    $proof['appContainerPeCharacteristicObserved'] = $true
    $proof['observedDllDependencies'] = $dependencies
    Save-Proof
    Write-Host 'Compiled/linked the existing native SDL WinRT DLL. No app, GPU, Wine or Xbox execution was performed.'
} catch {
    $proof['status'] = 'failed'
    $proof['failure'] = $_.Exception.Message
    Save-Proof
    throw
}
