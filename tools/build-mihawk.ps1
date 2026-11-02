<# Build a fresh native UWP Mihawk desktop using the pinned, source-built bridge.
No original desktop/Gecko/.NET payload is repackaged. Development signing only. #>
[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchain = & (Join-Path $PSScriptRoot 'build-uwp.ps1') -CheckOnly
$bridgeRoot = Join-Path $repoRoot "artifacts/upstream-bridge-x86/$Configuration"
$bridges = @(Get-ChildItem $bridgeRoot -Recurse -File -Filter 'Win32Bridge.Runtime.dll')
if ($bridges.Count -ne 1) { throw 'Build exactly one patched Win32Bridge before packaging Mihawk.' }
$bridge = $bridges[0].Directory.FullName
$winmd = Join-Path $bridge 'Win32Bridge.winmd'
if (-not (Test-Path $winmd)) { throw 'Bridge metadata is missing.' }
$output = Join-Path $repoRoot 'artifacts/mihawk-desktop'
$payload = Join-Path $output 'payload'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$arguments = @((Join-Path $repoRoot 'desktop/native/MihawkDesktop.vcxproj'), '/t:Rebuild', '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:WindowsTargetPlatformVersion=$($toolchain.WindowsSdkVersion)",
    "/p:BridgeWinmdPath=$winmd", '/p:AppxPackage=false', '/p:GenerateAppxPackageOnBuild=false',
    "/flp:logfile=$(Join-Path $output 'host-build.log');verbosity=normal")
& $toolchain.MSBuild @arguments
if ($LASTEXITCODE -ne 0) { throw "Mihawk host compilation failed: $LASTEXITCODE" }
$hostExe = Join-Path $repoRoot "artifacts/mihawk-host/bin/x64/$Configuration/MihawkDesktop.exe"
if (-not (Test-Path $hostExe)) { throw 'No native desktop executable was produced.' }
if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
& python (Join-Path $PSScriptRoot 'stage-mihawk.py') --output $payload --host $hostExe --bridge $bridge
if ($LASTEXITCODE -ne 0) { throw 'Mihawk asset staging failed.' }
[xml]$manifest = Get-Content -Raw (Join-Path $payload 'AppxManifest.xml')
$version = $manifest.Package.Identity.Version
$publisher = $manifest.Package.Identity.Publisher
$package = Join-Path $output "MihawkOS-Xbox_${version}_x64_DEV-SIGNED.msix"
$publicCertificate = Join-Path $output 'MihawkOS-Community-Development.cer'
$sdkBin = Join-Path $toolchain.WindowsSdkRoot "bin/$($toolchain.WindowsSdkVersion)/x64"
function Invoke-Sdk([string]$Name, [string[]]$Arguments, [string]$Log) {
    & (Join-Path $sdkBin $Name) @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $output $Log) | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "$Name failed: $LASTEXITCODE" }
}
$certificate = $null
try {
    Invoke-Sdk 'makeappx.exe' @('pack','/d',$payload,'/p',$package,'/o') 'makeappx.log'
    $certificate = New-SelfSignedCertificate -Type Custom -Subject $publisher `
        -FriendlyName 'Mihawk Xbox temporary community development certificate' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -KeyExportPolicy NonExportable `
        -HashAlgorithm SHA256 -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(12) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}')
    Export-Certificate -Cert $certificate -FilePath $publicCertificate | Out-Null
    Invoke-Sdk 'signtool.exe' @('sign','/fd','SHA256','/sha1',$certificate.Thumbprint,'/s','My','/d','Mihawk Xbox Community Development',$package) 'sign.log'
    Import-Certificate -FilePath $publicCertificate -CertStoreLocation 'Cert:\LocalMachine\Root' -Confirm:$false | Out-Null
    Invoke-Sdk 'signtool.exe' @('verify','/pa','/all','/v',$package) 'verify-signature.log'
    $sourceSha = (& git -C $repoRoot rev-parse HEAD).Trim()
    [ordered]@{
        PackageType='COMMUNITY-DEVELOPER-TEST'; Version=$version; Architecture='x64'; SourceCommit=$sourceSha
        MihawkPin='f81a028a668b36f5bcb51b09e66d526979382f1d'; PackageIdentity=$manifest.Package.Identity.Name
        Publisher=$publisher; EntryPoint='MihawkXbox.App'; Executable='MihawkDesktop.exe'
        PackageSha256=(Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant()
        BridgeSha256=(Get-FileHash (Join-Path $bridge 'Win32Bridge.Runtime.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
        BridgeWinmdSha256=(Get-FileHash $winmd -Algorithm SHA256).Hash.ToLowerInvariant()
        HostSha256=(Get-FileHash $hostExe -Algorithm SHA256).Hash.ToLowerInvariant()
        PayloadFiles=@(Get-ChildItem $payload -Recurse -File).Count
        SignatureVerification='SignTool /pa /all; temporary LocalMachine Root trust removed in finally'
        XboxHardwareTested=$false
        Note='Native platform WebView desktop; no PS5 Wine runtime or game compatibility implied.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $output 'build-info.json')
} finally {
    if ($null -ne $certificate) {
        try {
            if (Test-Path "Cert:\LocalMachine\Root\$($certificate.Thumbprint)") {
                Remove-Item "Cert:\LocalMachine\Root\$($certificate.Thumbprint)" -Force -Confirm:$false
            }
        } finally { Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -Force }
    }
    if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
}
Write-Host "Built $package"
