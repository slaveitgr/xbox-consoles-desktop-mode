# SPDX-License-Identifier: GPL-2.0-only
# A desktop Windows execution test of APP-family SDL, not AppContainer activation.
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -winsdk=10.0.26100.0'
$clang = (Get-Command clang-cl.exe).Source
$storeCrt = Join-Path $env:VCToolsInstallDir 'lib\x64\store'
$env:LIB = $storeCrt + ';' + $env:LIB
$source = (Resolve-Path external/Boxedwine64-full).Path
New-Item -ItemType Directory -Force artifacts/sdlvideo | Out-Null
Remove-Item artifacts/sdlvideo/result.json -ErrorAction SilentlyContinue
cmake -S backend/sdlvideo -B build/sdlvideo-app -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DBOXEDWINE_SOURCE=$source" -DPROTON_SDLVIDEO_APP_HEADERS=ON "-DPROTON_UWP_CRT_DIR=$storeCrt" 2>&1 | Tee-Object artifacts/sdlvideo/configure.log
if ($LASTEXITCODE -ne 0) { throw 'SDL APP-family configure failed.' }
cmake --build build/sdlvideo-app --target proton-sdlvideo-tests --parallel 4 -- -k 0 2>&1 | Tee-Object artifacts/sdlvideo/build.log
if ($LASTEXITCODE -ne 0) { throw 'Real SDL APP-family build failed.' }
$exe = (Resolve-Path build/sdlvideo-app/proton-sdlvideo-tests.exe).Path
& dumpbin /imports $exe | Set-Content artifacts/sdlvideo/imports.txt
if ($LASTEXITCODE -ne 0) { throw 'SDL import inspection failed.' }
$imports = Get-Content artifacts/sdlvideo/imports.txt -Raw
if ($imports -match '(?im)^\s*(?:VCRUNTIME[0-9_]+|MSVCP[0-9_]+|CONCRT[0-9_]+)\.dll\s*$') { throw 'SDL test accidentally linked the desktop C++ runtime.' }
if ($imports -notmatch '(?i)(?:VCRUNTIME|MSVCP)[0-9_]+_APP\.dll') { throw 'SDL test lacks the selected APP C++ runtime.' }
# The desktop test process needs private runtime DLLs. Stage only official
# UWP framework payloads from the VS/SDK installation, never install globally.
$roots = @((Join-Path $vsPath 'VC\Redist'),
    "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs",
    "${env:ProgramFiles(x86)}\Windows Kits\10\ExtensionSDKs")
$candidates = @($roots | Where-Object { Test-Path $_ } | ForEach-Object {
    Get-ChildItem $_ -Recurse -File -Filter Microsoft.VCLibs.x64.14.00.appx -ErrorAction SilentlyContinue
})
Add-Type -AssemblyName System.IO.Compression.FileSystem
$selected = $null
foreach ($package in ($candidates | Sort-Object LastWriteTimeUtc -Descending)) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($package.FullName)
    try {
        $manifest = $archive.GetEntry('AppxManifest.xml')
        if (-not $manifest) { continue }
        $reader = [System.IO.StreamReader]::new($manifest.Open())
        try { [xml]$identity = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($identity.Package.Identity.Name -ne 'Microsoft.VCLibs.140.00' -or $identity.Package.Identity.ProcessorArchitecture -ne 'x64') { continue }
        $count = 0
        foreach ($entry in $archive.Entries) {
            if ($entry.Name -notmatch '(?i)^(?:vcruntime|msvcp|concrt|vccorlib)[0-9_a-z]*_app\.dll$') { continue }
            $destination = Join-Path (Split-Path $exe) $entry.Name
            $entryInput = $entry.Open(); $entryOutput = [System.IO.File]::Create($destination)
            try { $entryInput.CopyTo($entryOutput) } finally { $entryInput.Dispose(); $entryOutput.Dispose() }
            $count++
        }
        if ($count -lt 3) { throw 'Official VCLibs package lacks the required APP runtime payloads.' }
        $selected = $package; break
    } finally { $archive.Dispose() }
}
if (-not $selected) { throw 'Official x64 UWP VCLibs framework was not found; no desktop CRT fallback is permitted.' }
ctest --test-dir build/sdlvideo-app --output-on-failure 2>&1 | Tee-Object artifacts/sdlvideo/tests.log
if ($LASTEXITCODE -ne 0) { throw 'Actual APP-family SDL software pipeline tests failed.' }
@{
    status = 'passed-desktop-windows-app-family'
    sourceCommit = $env:GITHUB_SHA
    executableSha256 = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    officialRuntimePackageSha256 = (Get-FileHash $selected.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    actualSoftwarePixelsTested = $true
    appCrtSelected = $true
    appContainerActivated = $false
    gpuPresentationTested = $false
    wineExecuted = $false
    xboxTested = $false
} | ConvertTo-Json | Set-Content artifacts/sdlvideo/result.json
