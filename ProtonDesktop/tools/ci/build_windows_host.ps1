# SPDX-License-Identifier: GPL-2.0-only
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2022 C++ is required.' }
$uwpPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.ComponentGroup.UWP.VC -property installationPath
$sdkHeaders = "${env:ProgramFiles(x86)}\Windows Kits\10\Include\10.0.26100.0\cppwinrt\winrt\base.h"
if (-not $uwpPath -or -not (Test-Path $sdkHeaders)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
    $installArgs = @('modify', '--installPath', "`"$vsPath`"", '--channelId', 'VisualStudio.17.Release',
        '--add', 'Microsoft.VisualStudio.ComponentGroup.UWP.VC',
        '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100', '--quiet', '--norestart')
    $process = Start-Process -FilePath $installer -ArgumentList $installArgs -Wait -PassThru
    if ($process.ExitCode -notin @(0, 3010)) { throw "VS prerequisite installation failed: $($process.ExitCode)" }
}
if (-not (Test-Path $sdkHeaders)) { throw 'Pinned Windows SDK C++/WinRT headers are missing.' }
New-Item -ItemType Directory -Force artifacts/windows-host | Out-Null
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild ProtonDesktop.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:AppxPackage=false /p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false /fl '/flp:logfile=artifacts/windows-host/build.log;verbosity=normal'
if ($LASTEXITCODE -ne 0) { throw 'ProtonDesktop UWP host compilation failed.' }
$exe = Get-Item build/uwp/Release/ProtonDesktop.exe
@{
    status = 'compiled'
    sourceCommit = $env:GITHUB_SHA
    windowsSdk = '10.0.26100.0'
    executableSha256 = (Get-FileHash $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    uwpExecuted = $false
    boxedwineLinked = $false
    wineExecuted = $false
    xboxTested = $false
} | ConvertTo-Json | Set-Content artifacts/windows-host/host-result.json
