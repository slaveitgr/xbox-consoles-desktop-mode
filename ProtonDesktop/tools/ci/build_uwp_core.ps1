# SPDX-License-Identifier: GPL-2.0-only
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
New-Item -ItemType Directory -Force artifacts/uwp-core | Out-Null
$source = (Resolve-Path external/Boxedwine64-full).Path
& $clang --version | Set-Content artifacts/uwp-core/compiler.txt
cmake -S tools/uwp-core -B build/uwp-core -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DBOXEDWINE_SOURCE=$source" 2>&1 | Tee-Object artifacts/uwp-core/configure.log
if ($LASTEXITCODE -ne 0) { throw 'UWP interpreter configure failed.' }
cmake --build build/uwp-core --target proton-uwp-core-linkcheck --parallel 4 -- -k 0 2>&1 | Tee-Object artifacts/uwp-core/build.log
if ($LASTEXITCODE -ne 0) { throw 'UWP interpreter compile/link failed; remaining services must be implemented.' }
dumpbin /headers build/uwp-core/proton-uwp-core-linkcheck.exe | Set-Content artifacts/uwp-core/headers.txt
if ($LASTEXITCODE -ne 0) { throw 'Header inspection failed.' }
dumpbin /imports build/uwp-core/proton-uwp-core-linkcheck.exe | Set-Content artifacts/uwp-core/imports.txt
if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed.' }
@{
    status = 'linked'
    sourceCommit = $env:GITHUB_SHA
    executableSha256 = (Get-FileHash build/uwp-core/proton-uwp-core-linkcheck.exe -Algorithm SHA256).Hash.ToLowerInvariant()
    apiFamily = 'WINAPI_FAMILY_APP'
    importsAudited = $false
    uwpExecuted = $false
    wineExecuted = $false
    xboxTested = $false
} | ConvertTo-Json | Set-Content artifacts/uwp-core/link-result.json
