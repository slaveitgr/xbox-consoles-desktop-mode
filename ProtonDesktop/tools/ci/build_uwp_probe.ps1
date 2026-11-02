# SPDX-License-Identifier: GPL-2.0-only
# Build/package an opt-in diagnostic CoreApplication; never starts Wine or deploys.
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'VS 2022 C++ is required.' }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -winsdk=10.0.26100.0'
New-Item -ItemType Directory -Force artifacts/uwp-probe | Out-Null
Remove-Item artifacts/uwp-probe/build-result.json -ErrorAction SilentlyContinue
python tools/make_elf64_smoke.py --output-dir DiagnosticPayloads
if ($LASTEXITCODE -ne 0) { throw 'Self-authored payload generation failed.' }
$libDir = (Resolve-Path artifacts/uwp-core/native-libs).Path
$builtins = (Get-Content artifacts/uwp-core/clang-builtins-path.txt -Raw).Trim()
if (-not (Test-Path $builtins)) { throw 'The official compiler-rt archive is required.' }
& msbuild ProtonDesktop.vcxproj /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:ProtonCoreProbeEnabled=true" "/p:ProtonCoreLibDir=$libDir" "/p:ProtonClangBuiltins=$builtins" /p:AppxPackage=true /p:GenerateAppxPackageOnBuild=true /p:AppxPackageSigningEnabled=false /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:BuildAppxUploadPackageForUap=false /p:WindowsTargetPlatformVersion=10.0.26100.0 /v:minimal 2>&1 | Tee-Object artifacts/uwp-probe/build.log
if ($LASTEXITCODE -ne 0) { throw 'Actual core-linked UWP probe build/package failed.' }
$executables = @(Get-ChildItem build/uwp-core-probe/Release -Recurse -Filter ProtonDesktop.exe)
if ($executables.Count -ne 1) { throw 'Expected exactly one opt-in probe executable.' }
$executable = $executables[0].FullName
& dumpbin /imports $executable | Set-Content artifacts/uwp-probe/imports.txt
if ($LASTEXITCODE -ne 0) { throw 'Probe import inspection failed.' }
$imports = Get-Content artifacts/uwp-probe/imports.txt -Raw
if ($imports -match '(?im)^\s*(?:VCRUNTIME[0-9_]+|MSVCP[0-9_]+|CONCRT[0-9_]+)\.dll\s*$') {
    throw 'Desktop C++ runtime retained in the actual probe application.'
}
if ($imports -notmatch '(?i)(?:VCRUNTIME|MSVCP)[0-9_]+_APP\.dll') {
    throw 'The probe lacks the UWP C++ runtime import.'
}
$packages = @(Get-ChildItem artifacts/uwp-core-probe/packages -Recurse -File | Where-Object {
    $_.Extension -in @('.appx','.msix') -and $_.Name -like 'ProtonDesktop*'
})
if (-not $packages.Count) { throw 'Unsigned probe package was not produced.' }
$packageResults = @($packages | ForEach-Object {
    @{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
@{
    status = 'compiled-and-packaged-unsigned'
    sourceCommit = $env:GITHUB_SHA
    executableSha256 = (Get-FileHash $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    coreLinked = $true
    probeEnabled = $true
    packages = $packageResults
    appActivated = $false
    cpuTestsExecutedInApp = $false
    wineExecuted = $false
    xboxTested = $false
} | ConvertTo-Json -Depth 5 | Set-Content artifacts/uwp-probe/build-result.json
