# SPDX-License-Identifier: GPL-2.0-only
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'VS 2022 C++ is required.' }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$clang = (Get-Command clang-cl.exe -ErrorAction SilentlyContinue).Source
if (-not $clang) {
    $clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
}
if (-not (Test-Path $clang)) { throw 'clang-cl is required for the upstream __int128 code.' }
$env:PATH = (Split-Path $clang) + ';' + $env:PATH
New-Item -ItemType Directory -Force artifacts/windows-core | Out-Null
$source = (Resolve-Path external/Boxedwine64-full).Path
& $clang --version | Set-Content artifacts/windows-core/compiler.txt
cmake -S tools/windows-core -B build/windows-core -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DBOXEDWINE_SOURCE=$source" 2>&1 | Tee-Object artifacts/windows-core/configure.log
if ($LASTEXITCODE -ne 0) { throw 'Windows interpreter configure failed.' }
# Collect independent compiler errors in one attempt; the real target still
# fails if any translation unit or the final link fails.
cmake --build build/windows-core --target boxedwine64-windows --parallel 4 -- -k 0 2>&1 | Tee-Object artifacts/windows-core/build.log
if ($LASTEXITCODE -ne 0) { throw 'Windows interpreter compilation failed.' }
