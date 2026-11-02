<# Build a fresh native UWP XWindows OS desktop using the pinned, source-built bridge.
No original desktop/Gecko/.NET payload is repackaged. Development signing only. #>
[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchain = & (Join-Path $PSScriptRoot 'build-uwp.ps1') -CheckOnly
$bridgeRoot = Join-Path $repoRoot "artifacts/upstream-bridge-x86/$Configuration"
$bridges = @(Get-ChildItem $bridgeRoot -Recurse -File -Filter 'Win32Bridge.Runtime.dll')
if ($bridges.Count -ne 1) { throw 'Build exactly one patched Win32Bridge before packaging XWindows OS.' }
$bridge = $bridges[0].Directory.FullName
$winmd = Join-Path $bridge 'Win32Bridge.winmd'
if (-not (Test-Path $winmd)) { throw 'Bridge metadata is missing.' }
& python (Join-Path $PSScriptRoot 'prepare-webview2.py')
if ($LASTEXITCODE -ne 0) { throw 'Official WebView2 SDK preparation failed.' }
$webSdk = Join-Path $repoRoot 'artifacts/webview2-sdk'
$winUIWinmd = Join-Path $webSdk 'microsoft.ui.xaml/lib/uap10.0/Microsoft.UI.Xaml.winmd'
$webViewWinmd = Join-Path $webSdk 'microsoft.web.webview2/lib/Microsoft.Web.WebView2.Core.winmd'
$output = Join-Path $repoRoot 'artifacts/mihawk-desktop'
$payload = Join-Path $output 'payload'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$arguments = @((Join-Path $repoRoot 'desktop/native/MihawkDesktop.vcxproj'), '/t:Rebuild', '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:WindowsTargetPlatformVersion=$($toolchain.WindowsSdkVersion)",
    "/p:BridgeWinmdPath=$winmd", "/p:WinUIWinmdPath=$winUIWinmd", "/p:WebView2WinmdPath=$webViewWinmd", '/p:AppxPackage=false', '/p:GenerateAppxPackageOnBuild=false',
    "/flp:logfile=$(Join-Path $output 'host-build.log');verbosity=normal")
& $toolchain.MSBuild @arguments
if ($LASTEXITCODE -ne 0) { throw "XWindows OS host compilation failed: $LASTEXITCODE" }
$hostExe = Join-Path $repoRoot "artifacts/mihawk-host/bin/x64/$Configuration/MihawkDesktop.exe"
if (-not (Test-Path $hostExe)) { throw 'No native desktop executable was produced.' }
if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
& python (Join-Path $PSScriptRoot 'stage-mihawk.py') --output $payload --host $hostExe --bridge $bridge --webview2 $webSdk
if ($LASTEXITCODE -ne 0) { throw 'XWindows OS asset staging failed.' }
# Manual packaging must register the same WinRT factories that the official
# SDK MSBuild targets normally add to a generated Appx manifest.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
public static class MihawkWebFactories {
    public static string[] Read(string path) {
        using(var input=File.OpenRead(path)) using(var pe=new PEReader(input)) {
            var r=pe.GetMetadataReader(MetadataReaderOptions.None);
            var names=new List<string>();
            foreach(var h in r.TypeDefinitions) {
                var type=r.GetTypeDefinition(h); bool factory=false;
                foreach(var ah in type.GetCustomAttributes()) {
                    var attribute=r.GetCustomAttribute(ah);
                    if(attribute.Constructor.Kind!=HandleKind.MemberReference) continue;
                    var parent=r.GetMemberReference((MemberReferenceHandle)attribute.Constructor).Parent;
                    if(parent.Kind!=HandleKind.TypeReference) continue;
                    var attributeType=r.GetTypeReference((TypeReferenceHandle)parent);
                    var name=r.GetString(attributeType.Name);
                    if(name=="ActivatableAttribute" || name=="StaticAttribute" || name=="ComposableAttribute") factory=true;
                }
                if(factory && r.GetString(type.Namespace)=="Microsoft.Web.WebView2.Core")
                    names.Add(r.GetString(type.Namespace)+"."+r.GetString(type.Name));
            }
            if(names.Count==0) throw new InvalidDataException("No WebView2 WinRT factories found.");
            if(!names.Contains("Microsoft.Web.WebView2.Core.CoreWebView2Controller")) names.Add("Microsoft.Web.WebView2.Core.CoreWebView2Controller");
            names.Sort(StringComparer.Ordinal); return names.ToArray();
        }
    }
}
'@
[xml]$generatedManifest = Get-Content -Raw (Join-Path $payload 'AppxManifest.xml')
$manifestNamespace = $generatedManifest.DocumentElement.NamespaceURI
$extensions = $generatedManifest.Package.Extensions
$extension = $generatedManifest.CreateElement('Extension',$manifestNamespace)
$extension.SetAttribute('Category','windows.activatableClass.inProcessServer')
$server = $generatedManifest.CreateElement('InProcessServer',$manifestNamespace)
$dllPath = $generatedManifest.CreateElement('Path',$manifestNamespace)
$dllPath.InnerText = 'Microsoft.Web.WebView2.Core.dll'
$server.AppendChild($dllPath) | Out-Null
$factories = [MihawkWebFactories]::Read($webViewWinmd)
foreach ($factory in $factories) {
    $class = $generatedManifest.CreateElement('ActivatableClass',$manifestNamespace)
    $class.SetAttribute('ActivatableClassId',$factory)
    $class.SetAttribute('ThreadingModel','both')
    $server.AppendChild($class) | Out-Null
}
$extension.AppendChild($server) | Out-Null
$extensions.AppendChild($extension) | Out-Null
$generatedManifest.Save((Join-Path $payload 'AppxManifest.xml'))
# Refresh hashes after generated activation registrations are written.
& python -c "from pathlib import Path;import hashlib,json,sys;p=Path(sys.argv[1]);rows={str(f.relative_to(p)).replace(chr(92),'/'):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(p.rglob('*')) if f.is_file()};(p.parent/'payload-files.json').write_text(json.dumps(rows,indent=2))" $payload
if ($LASTEXITCODE -ne 0) { throw 'Payload hash generation failed.' }
[xml]$manifest = Get-Content -Raw (Join-Path $payload 'AppxManifest.xml')
$version = $manifest.Package.Identity.Version
$publisher = $manifest.Package.Identity.Publisher
$package = Join-Path $output "XWindows-OS-Xbox_${version}_x64_DEV-SIGNED.msix"
$publicCertificate = Join-Path $output 'XWindows-OS-Community-Development.cer'
$sdkBin = Join-Path $toolchain.WindowsSdkRoot "bin/$($toolchain.WindowsSdkVersion)/x64"
function Invoke-Sdk([string]$Name, [string[]]$Arguments, [string]$Log) {
    & (Join-Path $sdkBin $Name) @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $output $Log) | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "$Name failed: $LASTEXITCODE" }
}
$certificate = $null
try {
    Invoke-Sdk 'makeappx.exe' @('pack','/d',$payload,'/p',$package,'/o') 'makeappx.log'
    $certificate = New-SelfSignedCertificate -Type Custom -Subject $publisher `
        -FriendlyName 'XWindows OS temporary community development certificate' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -KeyExportPolicy NonExportable `
        -HashAlgorithm SHA256 -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(12) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}')
    Export-Certificate -Cert $certificate -FilePath $publicCertificate | Out-Null
    Invoke-Sdk 'signtool.exe' @('sign','/fd','SHA256','/sha1',$certificate.Thumbprint,'/s','My','/d','XWindows OS Community Development',$package) 'sign.log'
    Import-Certificate -FilePath $publicCertificate -CertStoreLocation 'Cert:\LocalMachine\Root' -Confirm:$false | Out-Null
    Invoke-Sdk 'signtool.exe' @('verify','/pa','/all','/v',$package) 'verify-signature.log'
    $sourceSha = (& git -C $repoRoot rev-parse HEAD).Trim()
    [ordered]@{
        ProductName='XWindows OS'; PackageType='COMMUNITY-DEVELOPER-TEST'; Version=$version; Architecture='x64'; SourceCommit=$sourceSha
        MihawkPin='f81a028a668b36f5bcb51b09e66d526979382f1d'; PackageIdentity=$manifest.Package.Identity.Name
        Publisher=$publisher; EntryPoint='MihawkXbox.App'; Executable='MihawkDesktop.exe'
        PackageSha256=(Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant()
        BridgeSha256=(Get-FileHash (Join-Path $bridge 'Win32Bridge.Runtime.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
        BridgeWinmdSha256=(Get-FileHash $winmd -Algorithm SHA256).Hash.ToLowerInvariant()
        HostSha256=(Get-FileHash $hostExe -Algorithm SHA256).Hash.ToLowerInvariant()
        PayloadFiles=@(Get-ChildItem $payload -Recurse -File).Count
        WebView2Sdk='1.0.3719.77'; WinUI='2.8.7'; WebView2FactoryCount=$factories.Count
        SignatureVerification='SignTool /pa /all; temporary LocalMachine Root trust removed in finally'
        XboxHardwareTested=$false
        Note='XWindows OS WebView2 desktop; no PS5 Wine runtime or game compatibility implied.'
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
