<#
.SYNOPSIS
Build the native x64 UWP diagnostics companion and create an AppX package.
.DESCRIPTION
Requires Windows, Visual Studio 2022 with C++ UWP tools, and SDK 22621 or later.
Output: artifacts/diagnostics/packages/{Configuration}/{unsigned|test-signed}/.
The default package is unsigned. -Sign uses a temporary, nonexportable CN=SlaveIT
certificate; only its public .cer is exported. The certificate and private key
are removed from the user certificate store when the command finishes.
This builds only the diagnostic app, not the upstream desktop shell.
.EXAMPLE
pwsh -File tools/build-uwp.ps1 -Configuration Release -Sign
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$WindowsSdkVersion,
    [switch]$Sign,
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'UWP packaging requires Windows and Visual Studio 2022. Run the portable CMake tests on other hosts.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio 2022 was not found. Install VS 2022 and its C++ Universal Windows Platform tools (Microsoft.VisualStudio.ComponentGroup.UWP.VC).'
}
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.ComponentGroup.UWP.VC -property installationPath
if (-not $vsPath) {
    throw 'Install Visual Studio 2022 C++ UWP tools: Microsoft.VisualStudio.ComponentGroup.UWP.VC and Microsoft.VisualStudio.Component.VC.Tools.x86.x64. A desktop C++ workload alone is insufficient.'
}
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild was not found at $msbuild." }

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkRegistry = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
if ($null -ne $sdkRegistry -and $sdkRegistry.PSObject.Properties.Name -contains 'KitsRoot10') {
    $sdkRoot = $sdkRegistry.KitsRoot10
}
if (-not $WindowsSdkVersion) {
    $availableSdks = @(Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and [version]$_.Name -ge [version]'10.0.22621.0' } |
        Sort-Object { [version]$_.Name } -Descending)
    foreach ($sdk in $availableSdks) {
        if ((Test-Path (Join-Path $sdk.FullName 'cppwinrt\winrt\base.h')) -and
            (Test-Path (Join-Path $sdkRoot "bin\$($sdk.Name)\x64\makeappx.exe"))) {
            $WindowsSdkVersion = $sdk.Name
            break
        }
    }
}
if (-not $WindowsSdkVersion -or [version]$WindowsSdkVersion -lt [version]'10.0.22621.0') {
    throw 'Install Windows SDK 10.0.22621.0 or newer, including C++/WinRT headers and AppX packaging tools. Optionally select it with -WindowsSdkVersion.'
}
foreach ($requiredPath in @(
    "Include\$WindowsSdkVersion\cppwinrt\winrt\base.h",
    "Lib\$WindowsSdkVersion\um\x64\WindowsApp.lib",
    "bin\$WindowsSdkVersion\x64\makeappx.exe")) {
    if (-not (Test-Path (Join-Path $sdkRoot $requiredPath))) {
        throw "Windows SDK $WindowsSdkVersion is incomplete: missing $requiredPath. Modify the SDK installation or select another installed version."
    }
}

Write-Host "Visual Studio: $vsPath"
Write-Host "Windows SDK: $WindowsSdkVersion; toolset: v143; architecture: x64"
if ($CheckOnly) {
    [pscustomobject]@{ MSBuild = $msbuild; VisualStudio = $vsPath; WindowsSdkVersion = $WindowsSdkVersion; WindowsSdkRoot = $sdkRoot }
    return
}

$packageKind = if ($Sign) { 'test-signed' } else { 'unsigned' }
$packageDir = Join-Path $repoRoot "artifacts\diagnostics\packages\$Configuration\$packageKind"
$logDir = Join-Path $repoRoot "artifacts\diagnostics\logs\$Configuration\$packageKind"
New-Item -ItemType Directory -Force -Path $packageDir, $logDir | Out-Null
$arguments = @(
    (Join-Path $repoRoot 'app\RuntimeDiagnostics.vcxproj'),
    '/t:Rebuild', '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64',
    "/p:WindowsTargetPlatformVersion=$WindowsSdkVersion",
    '/p:GenerateAppxPackageOnBuild=true', '/p:UapAppxPackageBuildMode=SideloadOnly',
    '/p:AppxBundle=Never', '/p:AppxBundlePlatforms=x64',
    "/p:AppxPackageDir=$packageDir\", '/p:AppxAutoIncrementPackageRevision=false',
    "/bl:$(Join-Path $logDir 'build.binlog')",
    "/flp:logfile=$(Join-Path $logDir 'build.log');verbosity=normal"
)
$certificate = $null
try {
    if ($Sign) {
        $certificate = New-SelfSignedCertificate -Type Custom -Subject 'CN=SlaveIT' `
            -FriendlyName 'Xbox Desktop Diagnostics temporary development certificate' `
            -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 `
            -KeyExportPolicy NonExportable -HashAlgorithm SHA256 `
            -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(12) `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        Export-Certificate -Cert $certificate -FilePath (Join-Path $packageDir 'SlaveIT-Development.cer') | Out-Null
        $arguments += @('/p:AppxPackageSigningEnabled=true', "/p:PackageCertificateThumbprint=$($certificate.Thumbprint)", '/p:PackageCertificateKeyFile=')
        Write-Host 'Creating a test-signed AppX. The exported .cer contains the public certificate only.'
    } else {
        $arguments += @('/p:AppxPackageSigningEnabled=false', '/p:PackageCertificateThumbprint=', '/p:PackageCertificateKeyFile=')
    }
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "UWP build failed (MSBuild exit $LASTEXITCODE). See $logDir." }
    $packages = @(Get-ChildItem $packageDir -Recurse -File -Filter '*.appx')
    if ($packages.Count -eq 0) { throw "Build finished without producing an AppX under $packageDir." }
    [pscustomobject]@{
        PackageType = $packageKind
        Configuration = $Configuration
        Architecture = 'x64'
        WindowsSdk = $WindowsSdkVersion
        Publisher = 'CN=SlaveIT'
        CertificateThumbprint = $(if ($certificate) { $certificate.Thumbprint } else { $null })
        XboxHardwareTested = $false
        Note = 'Diagnostic companion only. A package build does not establish Win32 application or game compatibility.'
    } | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $packageDir 'build-info.json')
    Write-Host "Packages: $packageDir"
    Write-Host "Build logs: $logDir"
    Write-Host 'Physical Xbox validation is still required.'
} finally {
    if ($null -ne $certificate) {
        Remove-Item -Path "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -ErrorAction Stop
    }
}
