# SPDX-License-Identifier: GPL-2.0-only
# Real Wine session compile/link only; no host or guest execution.
param([string]$SdlWinrtDir = 'build/sdl-winrt/out')
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'VS 2022 C++ is required.' }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -winsdk=10.0.26100.0'
$clang = (Get-Command clang-cl.exe -ErrorAction SilentlyContinue).Source
if (-not $clang) { $clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe' }
if (-not (Test-Path $clang)) { throw 'clang-cl is required.' }
$env:PATH = (Split-Path $clang) + ';' + $env:PATH
New-Item -ItemType Directory -Force artifacts/wine-core | Out-Null
Remove-Item artifacts/wine-core/link-result.json -ErrorAction SilentlyContinue
$storeCrt = Join-Path $env:VCToolsInstallDir 'lib\x64\store'
if (-not (Test-Path (Join-Path $storeCrt 'msvcrt.lib')) -or -not (Test-Path (Join-Path $storeCrt 'msvcprt.lib'))) {
    throw 'The x64 UWP CRT import libraries are required; desktop CRT must not substitute.'
}
# Ninja does not select VS UWP library directories through a .vcxproj.
$env:LIB = $storeCrt + ';' + $env:LIB
$storeCrt | Set-Content artifacts/wine-core/store-crt.txt
$source = (Resolve-Path external/Boxedwine64-full).Path
$sdl = (Resolve-Path $SdlWinrtDir).Path
if (-not (Test-Path (Join-Path $sdl 'SDL2.dll')) -or -not (Test-Path (Join-Path $sdl 'SDL2.lib'))) { throw 'The actual native SDL WinRT DLL/import library is required.' }
& $clang --version | Set-Content artifacts/wine-core/compiler.txt
cmake -S tools/uwp-core -B build/wine-core -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DBOXEDWINE_SOURCE=$source" "-DPROTON_UWP_CRT_DIR=$storeCrt" -DPROTON_ENABLE_WINE_SESSION=ON "-DPROTON_SDL_WINRT_DIR=$sdl" 2>&1 | Tee-Object artifacts/wine-core/configure.log
if ($LASTEXITCODE -ne 0) { throw 'UWP interpreter configure failed.' }
cmake --build build/wine-core --target proton-wine-linkcheck proton-wine-core protondesktop_zip --parallel 4 -- -k 0 2>&1 | Tee-Object artifacts/wine-core/build.log
if ($LASTEXITCODE -ne 0) { throw 'UWP interpreter compile/link failed; remaining services must be implemented.' }
dumpbin /headers build/wine-core/proton-wine-linkcheck.exe | Set-Content artifacts/wine-core/headers.txt
if ($LASTEXITCODE -ne 0) { throw 'Header inspection failed.' }
dumpbin /imports build/wine-core/proton-wine-linkcheck.exe | Set-Content artifacts/wine-core/imports.txt
if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed.' }
$headers = Get-Content artifacts/wine-core/headers.txt -Raw
$imports = Get-Content artifacts/wine-core/imports.txt -Raw
if ($imports -notmatch '(?im)^\s*SDL2\.dll\s*$') { throw 'Full Wine entry failed to retain the native SDL WinRT import.' }
if ($headers -notmatch 'App Container') { throw 'The diagnostic lacks the APP container image flag.' }
if ($imports -match '(?im)^\s*(?:VCRUNTIME[0-9_]+|MSVCP[0-9_]+|CONCRT[0-9_]+)\.dll\s*$') {
    throw 'Desktop C++ runtime imported; UWP CRT selection failed.'
}
if ($imports -notmatch '(?i)(?:VCRUNTIME|MSVCP)[0-9_]+_APP\.dll') {
    throw 'No UWP C++ runtime import was found; inspect CRT selection.'
}
$forbidden = @('CreateProcessA','CreateProcessW','WinExec','ShellExecuteA','ShellExecuteW',
    'ShellExecuteExA','ShellExecuteExW','MessageBoxA','MessageBoxW','SetProcessDPIAware',
    'NtQuerySystemInformation','VirtualAlloc','VirtualProtect','LoadLibraryA','LoadLibraryW')
foreach ($name in $forbidden) {
    if ($imports -match ('(?m)^\s*[0-9A-Fa-f]+\s+' + [regex]::Escape($name) + '\s*$')) {
        throw "Desktop/profile-forbidden import retained: $name"
    }
}
@{
    status = 'linked-wine-session-graph'
    sourceCommit = $env:GITHUB_SHA
    executableSha256 = (Get-FileHash build/wine-core/proton-wine-linkcheck.exe -Algorithm SHA256).Hash.ToLowerInvariant()
    apiFamily = 'WINAPI_FAMILY_APP'
    importsAudited = $false
    basicImportGuardPassed = $true
    appCrtSelected = $true
    scope = 'Real WineSession/kernel/VFS/native SDL retained by link entry; execution and full API certification pending'
    actualWineSessionEntryLinked = $true
    nativeSdlWinrtImported = $true
    zipSupportLinked = $true
    uwpExecuted = $false
    wineExecuted = $false
    xboxTested = $false
} | ConvertTo-Json | Set-Content artifacts/wine-core/link-result.json

# Keep explicit archive names; never glob in desktop objects or the LinkProbe entry.
$nativeLibs = New-Item -ItemType Directory -Force artifacts/wine-core/native-libs
foreach ($path in Get-Content build/wine-core/native-library-paths.txt) {
    if (-not (Test-Path $path)) { throw "Missing actual core archive: $path" }
    Copy-Item $path $nativeLibs.FullName -Force
}
Copy-Item build/wine-core/clang-builtins-path.txt artifacts/wine-core/clang-builtins-path.txt -Force
