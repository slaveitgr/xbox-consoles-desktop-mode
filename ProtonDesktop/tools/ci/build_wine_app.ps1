# SPDX-License-Identifier: GPL-2.0-only
# Build/package the real SDL/WinRT Wine entry. Never uploads, installs or runs it.
param(
    [string]$Source = 'external/Boxedwine64-full',
    [string]$WineLibDir = 'artifacts/wine-core/native-libs',
    [string]$SdlWinrtDir = 'build/sdl-winrt/out',
    [string]$ClangBuiltins = '',
    [string]$SdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$artifacts = Join-Path $repoRoot 'artifacts/uwp-wine'
New-Item -ItemType Directory -Force $artifacts | Out-Null
$proof = [ordered]@{
    status = 'incomplete'; sourceCommit = $env:GITHUB_SHA
    backendPin = '58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4'
    entry = 'native-SDL-WinRT'; packageVersion = '0.2.0.1'
    appActivated = $false; wineExecuted = $false; desktopPixelsVerified = $false; xboxTested = $false
}
function Save-Proof { $proof | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $artifacts 'build-result.json') -Encoding utf8 }
Save-Proof
try {
    $sourceRoot = (Resolve-Path $Source).Path
    $actualPin = (& git -C $sourceRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualPin -ne $proof.backendPin) { throw 'The pinned Boxedwine64 SDL headers are required.' }
    $dirty = & git -C $sourceRoot status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'The upstream source checkout must remain pristine.' }
    $wineLibraries = (Resolve-Path $WineLibDir).Path
    $sdlLibraries = (Resolve-Path $SdlWinrtDir).Path
    $sdlIncludes = Join-Path $sourceRoot 'lib/sdl2/include'
    $libraryNames = @('proton-wine-core.lib', 'protondesktop_zip.lib', 'protondesktop_uwp_memory.lib',
        'protondesktop_uwp_host_services.lib', 'protondesktop_uwp_socket_pair.lib')
    foreach ($name in $libraryNames) {
        if (-not (Test-Path (Join-Path $wineLibraries $name))) { throw "Required actual Wine profile archive is missing: $name" }
    }
    foreach ($name in @('SDL2.dll', 'SDL2.lib')) {
        if (-not (Test-Path (Join-Path $sdlLibraries $name))) { throw "The real native SDL WinRT binary is missing: $name" }
    }
    if (-not $ClangBuiltins) {
        $ClangBuiltins = (Get-Content (Join-Path $repoRoot 'artifacts/uwp-core/clang-builtins-path.txt') -Raw).Trim()
    }
    $builtins = (Resolve-Path $ClangBuiltins).Path
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = (& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $vsPath) { throw 'VS 2022 with C++/CX UWP and the selected Windows SDK is required.' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -winsdk=$SdkVersion"
    $storeCrt = Join-Path $env:VCToolsInstallDir 'lib/x64/store'
    foreach ($name in @('msvcrt.lib', 'msvcprt.lib', 'vccorlib.lib')) {
        if (-not (Test-Path (Join-Path $storeCrt $name))) { throw "Missing genuine UWP/C++CX Store runtime library: $name" }
    }
    $env:LIB = $storeCrt + ';' + $env:LIB

    function Inspect-AppImage([string]$image, [string]$stem) {
        foreach ($mode in @('headers', 'imports')) {
            & dumpbin "/$mode" $image | Set-Content (Join-Path $artifacts "$stem-$mode.txt")
            if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $stem" }
        }
        $headers = Get-Content (Join-Path $artifacts "$stem-headers.txt") -Raw
        $imports = Get-Content (Join-Path $artifacts "$stem-imports.txt") -Raw
        if ($headers -notmatch '(?i)App Container') { throw "$stem lacks the APP_CONTAINER image characteristic." }
        $dependencies = @([regex]::Matches($imports, '(?im)^\s*([a-z0-9_.-]+\.dll)\s*$') |
            ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
        $desktop = @($dependencies | Where-Object {
            $_ -match '^(?:vcruntime|msvcp|concrt|vccorlib)[0-9_a-z]*\.dll$' -and $_ -notmatch '_app\.dll$'
        })
        if ($desktop.Count) { throw "$stem imported desktop C++ runtime libraries: $($desktop -join ', ')" }
        if ($imports -notmatch '(?i)(?:VCRUNTIME|MSVCP|VCCORLIB)[0-9_a-z]*_APP\.dll') {
            throw "$stem lacks a genuine APP C++ runtime import."
        }
        return ,$dependencies
    }
    $sdlDll = Join-Path $sdlLibraries 'SDL2.dll'
    $proof['sdlDependencies'] = Inspect-AppImage $sdlDll 'sdl'
    & dumpbin /exports $sdlDll | Set-Content (Join-Path $artifacts 'sdl-exports.txt')
    if ($LASTEXITCODE -ne 0) { throw 'SDL export inspection failed.' }
    $exports = Get-Content (Join-Path $artifacts 'sdl-exports.txt') -Raw
    foreach ($symbol in @('SDL_WinRTRunApp', 'SDL_CreateWindow', 'SDL_CreateRenderer', 'SDL_RenderPresent')) {
        if ($exports -notmatch "(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+$symbol(?:\s|$)") {
            throw "Native SDL WinRT DLL lacks its actual public C API export: $symbol"
        }
    }

    $project = Join-Path $repoRoot 'ProtonDesktop.Wine.vcxproj'
    $arguments = @($project, '/m:4', '/t:Rebuild', '/nologo', '/v:minimal',
        '/p:Configuration=Release', '/p:Platform=x64', "/p:WindowsTargetPlatformVersion=$SdkVersion",
        "/p:ProtonWineLibDir=$wineLibraries", "/p:ProtonSdlWinrtDir=$sdlLibraries",
        "/p:ProtonSdlIncludeDir=$sdlIncludes", "/p:ProtonClangBuiltins=$builtins",
        '/p:AppxPackage=true', '/p:GenerateAppxPackageOnBuild=true', '/p:AppxPackageSigningEnabled=false',
        '/p:AppxBundle=Never', '/p:UapAppxPackageBuildMode=SideloadOnly', '/p:BuildAppxUploadPackageForUap=false',
        "/bl:$(Join-Path $artifacts 'build.binlog')")
    $arguments | ConvertTo-Json | Set-Content (Join-Path $artifacts 'msbuild-arguments.json')
    & (Join-Path $vsPath 'MSBuild/Current/Bin/MSBuild.exe') @arguments 2>&1 | Tee-Object (Join-Path $artifacts 'build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Actual native SDL/WinRT Wine application build/package failed.' }
    # Genuine UWP targets may append the project name to OutDir. Inspect the
    # unique actual image rather than assuming a desktop-style flat layout.
    $outputRoot = Join-Path $repoRoot 'build/uwp-wine/Release'
    $executables = @(Get-ChildItem -LiteralPath $outputRoot -Recurse -File -Filter ProtonDesktop.Wine.exe)
    if ($executables.Count -ne 1) { throw 'Expected one actual native Wine application executable in its output tree.' }
    $executable = $executables[0].FullName
    $proof['emittedExecutableRelativePath'] = [System.IO.Path]::GetRelativePath($repoRoot, $executable)
    $proof['applicationDependencies'] = Inspect-AppImage $executable 'app'
    if ($proof.applicationDependencies -notcontains 'sdl2.dll') { throw 'The application did not link the real native SDL2.dll.' }
    $exeDigest = (Get-FileHash $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    $sdlDigest = (Get-FileHash $sdlDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $packages = @(Get-ChildItem (Join-Path $artifacts 'packages') -Recurse -File | Where-Object {
        $_.Extension -in @('.appx', '.msix') -and $_.Name -like 'ProtonDesktop*'
    })
    if (-not $packages.Count) { throw 'The actual unsigned Wine app package is missing.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $packageProof = @()
    foreach ($package in $packages) {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($package.FullName)
        try {
            $names = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
            if ($names -notcontains 'SDL2.dll' -or $names -notcontains 'ProtonDesktop.Wine.exe') {
                throw 'The generated package must actually contain the native SDL DLL and Wine application.'
            }
            foreach ($binary in @(@{name='SDL2.dll'; digest=$sdlDigest}, @{name='ProtonDesktop.Wine.exe'; digest=$exeDigest})) {
                $stream = $zip.GetEntry($binary.name).Open()
                $hash = [System.Security.Cryptography.SHA256]::Create()
                try { $digest = [System.BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
                finally { $hash.Dispose(); $stream.Dispose() }
                if ($digest -ne $binary.digest) { throw 'The package contains a stale or different native binary.' }
            }
            if (@($names | Where-Object { $_ -match '(?i)(?:^|/)(?:glibc-rootfs64|wine64|prefix64)\.zip$' }).Count) {
                throw 'Unqualified rootfs binaries must not be redistributed in this application package.'
            }
            $manifestEntry = $zip.GetEntry('AppxManifest.xml')
            if (-not $manifestEntry) { throw 'The generated package lacks its manifest.' }
            $reader = [System.IO.StreamReader]::new($manifestEntry.Open())
            try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
            if ($manifest.Package.Identity.Name -ne 'ProtonDesktop.Native' -or
                $manifest.Package.Identity.Publisher -ne 'CN=ProtonDesktopDev' -or
                $manifest.Package.Identity.Version -ne '0.2.0.1') {
                throw 'The generated package identity/version differs from the separate Wine manifest.'
            }
        } finally { $zip.Dispose() }
        $packageProof += [ordered]@{name=$package.Name; bytes=$package.Length; sha256=(Get-FileHash $package.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $proof['status'] = 'compiled-and-packaged-unsigned'
    $proof['executableSha256'] = $exeDigest
    $proof['sdlDllSha256'] = $sdlDigest
    $proof['archives'] = @($libraryNames | ForEach-Object {
        [ordered]@{name=$_; sha256=(Get-FileHash (Join-Path $wineLibraries $_) -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    $proof['packages'] = $packageProof
    Save-Proof
    Write-Host 'Built the real native SDL/WinRT Wine app package. No app/Wine/Xbox execution performed by this script.'
} catch {
    $proof['status'] = 'failed'; $proof['failure'] = $_.Exception.Message
    Save-Proof
    throw
}
