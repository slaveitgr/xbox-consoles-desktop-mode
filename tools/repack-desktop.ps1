<#
.SYNOPSIS
Replace the Win32Bridge component in a local DesktopMode x64 package.
.DESCRIPTION
Requires Windows, PowerShell 7, and Windows SDK MakeAppx/SignTool. The shell and
Gecko binaries come from SourcePackage; neither is rebuilt or downloaded here.
The original identity is retained and the version must increase. Public WinRT
metadata is compared before replacement. The output is a community developer
test package, signed with a temporary nonexportable certificate whose subject
matches the existing manifest publisher. Only its public certificate is saved.
.EXAMPLE
pwsh -File tools/repack-desktop.ps1 -SourcePackage C:\input\DesktopMode.msix `
  -BridgeDirectory C:\build\Win32Bridge.Runtime -OutputDirectory C:\output
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourcePackage,
    [Parameter(Mandatory)][string]$BridgeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Version = '1.0.46.1',
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSourceSha256,
    [string]$WindowsSdkVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'DesktopMode repackaging requires Windows and Windows SDK MakeAppx/SignTool.'
}
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Run this script with PowerShell 7 (pwsh).' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$source = (Resolve-Path -LiteralPath $SourcePackage).Path
$bridge = (Resolve-Path -LiteralPath $BridgeDirectory).Path
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw 'SourcePackage must be a local AppX/MSIX file.' }
if ([IO.Path]::GetExtension($source) -notin @('.appx', '.msix')) { throw 'Supply one x64 AppX/MSIX, not a bundle.' }
if (-not (Test-Path -LiteralPath $bridge -PathType Container)) { throw 'BridgeDirectory must be a directory.' }
try { $newVersion = [version]$Version } catch { throw 'Version must have four numeric components.' }
$versionParts = @($newVersion.Major, $newVersion.Minor, $newVersion.Build, $newVersion.Revision)
if ($newVersion.Revision -lt 0 -or @($versionParts | Where-Object { $_ -lt 0 -or $_ -gt 65535 }).Count -ne 0) {
    throw 'Version must contain four components in the range 0..65535.'
}
$Version = $newVersion.ToString(4)
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$package = Join-Path $output "DesktopMode_Bridge_${Version}_x64_DEVELOPER-TEST.msix"
$certificateFile = Join-Path $output "DesktopMode_Bridge_${Version}_DEVELOPER-TEST.cer"
foreach ($path in @($package, $certificateFile, (Join-Path $output 'repack-info.json'))) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path. Use a new output directory." }
}

function Invoke-SdkTool([string]$Tool, [string[]]$Arguments, [string]$LogFile) {
    & $Tool @Arguments *> $LogFile
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $LogFile -Tail 30 | Write-Host
        throw "$([IO.Path]::GetFileName($Tool)) failed with exit code $LASTEXITCODE. See $LogFile."
    }
}

function Read-SafeXml([IO.Stream]$Stream) {
    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($Stream, $settings)
    try {
        $document = [Xml.XmlDocument]::new()
        $document.PreserveWhitespace = $true
        $document.XmlResolver = $null
        $document.Load($reader)
        return ,$document
    } finally { $reader.Dispose() }
}

function Test-SourceBlockMap([string]$Path) {
    # Verify uncompressed 64 KiB block hashes before unpacking. This checks file
    # integrity; optional ExpectedSourceSha256 pins the supplied source artifact.
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry('AppxBlockMap.xml')
        if ($null -eq $entry) { throw 'Source package has no AppxBlockMap.xml.' }
        $stream = $entry.Open()
        try { $map = Read-SafeXml $stream } finally { $stream.Dispose() }
        if ($map.DocumentElement.GetAttribute('HashMethod') -cne 'http://www.w3.org/2001/04/xmlenc#sha256') {
            throw 'Source package uses an unsupported block-map hash method.'
        }
        $count = 0
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            foreach ($file in $map.DocumentElement.SelectNodes("*[local-name()='File']")) {
                $name = $file.GetAttribute('Name').Replace('\', '/')
                $item = $archive.GetEntry($name)
                if ($null -eq $item) {
                    # OPC ZIP part names escape reserved URI characters, while
                    # AppxBlockMap uses the decoded Windows filename (@ -> %40).
                    $partName = ($name.Split('/') | ForEach-Object { [Uri]::EscapeDataString($_) }) -join '/'
                    $item = $archive.GetEntry($partName)
                }
                if ($null -eq $item) { throw "Block-map file is missing: $name" }
                $inputStream = $item.Open()
                try {
                    $buffer = [byte[]]::new(65536)
                    [uint64]$total = 0
                    foreach ($block in $file.SelectNodes("*[local-name()='Block']")) {
                        $used = 0
                        while ($used -lt $buffer.Length) {
                            $read = $inputStream.Read($buffer, $used, $buffer.Length - $used)
                            if ($read -eq 0) { break }
                            $used += $read
                        }
                        $hash = [Convert]::ToBase64String($algorithm.ComputeHash($buffer, 0, $used))
                        if ($hash -cne $block.GetAttribute('Hash')) { throw "Source block hash mismatch: $name" }
                        $total += $used
                    }
                    if ($inputStream.ReadByte() -ne -1 -or $total -ne [uint64]$file.GetAttribute('Size')) {
                        throw "Source block-map size mismatch: $name"
                    }
                } finally { $inputStream.Dispose() }
                $count++
            }
        } finally { $algorithm.Dispose() }
        if ($count -eq 0) { throw 'Source block map has no files.' }
        return $count
    } finally { $archive.Dispose() }
}

function Find-SdkTools {
    try {
        $selection = & (Join-Path $PSScriptRoot 'build-uwp.ps1') -CheckOnly -WindowsSdkVersion $WindowsSdkVersion
        $candidate = Join-Path $selection.WindowsSdkRoot "bin\$($selection.WindowsSdkVersion)\x64"
        if ((Test-Path (Join-Path $candidate 'makeappx.exe')) -and (Test-Path (Join-Path $candidate 'signtool.exe'))) {
            return $candidate
        }
    } catch { Write-Verbose "Build toolchain lookup unavailable; checking installed SDK tools: $_" }
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $registry = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
    if ($null -ne $registry -and $registry.PSObject.Properties.Name -contains 'KitsRoot10') { $sdkRoot = $registry.KitsRoot10 }
    $candidates = @(Get-ChildItem (Join-Path $sdkRoot 'bin') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and (-not $WindowsSdkVersion -or $_.Name -eq $WindowsSdkVersion) } |
        Sort-Object { [version]$_.Name } -Descending)
    foreach ($directory in $candidates) {
        $candidate = Join-Path $directory.FullName 'x64'
        if ((Test-Path (Join-Path $candidate 'makeappx.exe')) -and (Test-Path (Join-Path $candidate 'signtool.exe'))) { return $candidate }
    }
    throw 'Install a Windows SDK containing x64 makeappx.exe and signtool.exe.'
}

function Test-BridgeDll([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 128 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw 'Bridge DLL has no DOS header.' }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($pe -gt $bytes.Length - 96 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw 'Bridge DLL has an invalid PE header.' }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b -or
        ([BitConverter]::ToUInt16($bytes, $pe + 22) -band 0x2000) -eq 0 -or
        ([BitConverter]::ToUInt16($bytes, $pe + 24 + 70) -band 0x1000) -eq 0) {
        throw 'Bridge DLL must be a PE32+ AMD64 AppContainer DLL.'
    }
}

# Compare public metadata, not PE timestamps/MVIDs. This detects changed WinRT
# interfaces, IIDs, method ordering, parameter signatures and properties before
# reusing the existing .NET Native shell and its compiled interface projections.
if (-not ('DesktopBridgeRepack.WinmdContract' -as [type])) {
    $metadataCode = @'
using System;
using System.IO;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;

namespace DesktopBridgeRepack {
public sealed class SignatureNames : ISignatureTypeProvider<string, object> {
    public string GetArrayType(string t, ArrayShape s) => t + "[rank=" + s.Rank + ";sizes=" + string.Join(",", s.Sizes) + ";lower=" + string.Join(",", s.LowerBounds) + "]";
    public string GetByReferenceType(string t) => t + "&";
    public string GetFunctionPointerType(MethodSignature<string> s) => "fn:" + Format(s);
    public string GetGenericInstantiation(string t, ImmutableArray<string> a) => t + "<" + string.Join(",", a) + ">";
    public string GetGenericMethodParameter(object c, int i) => "!!" + i;
    public string GetGenericTypeParameter(object c, int i) => "!" + i;
    public string GetModifiedType(string m, string t, bool required) => t + (required ? " modreq(" : " modopt(") + m + ")";
    public string GetPinnedType(string t) => t + " pinned";
    public string GetPointerType(string t) => t + "*";
    public string GetPrimitiveType(PrimitiveTypeCode c) => c.ToString();
    public string GetSZArrayType(string t) => t + "[]";
    public string GetTypeFromDefinition(MetadataReader r, TypeDefinitionHandle h, byte k) {
        var d = r.GetTypeDefinition(h); var parent = d.GetDeclaringType();
        return parent.IsNil ? r.GetString(d.Namespace) + "." + r.GetString(d.Name) : GetTypeFromDefinition(r, parent, k) + "+" + r.GetString(d.Name);
    }
    public string GetTypeFromReference(MetadataReader r, TypeReferenceHandle h, byte k) {
        var d = r.GetTypeReference(h);
        return d.ResolutionScope.Kind == HandleKind.TypeReference ? GetTypeFromReference(r, (TypeReferenceHandle)d.ResolutionScope, k) + "+" + r.GetString(d.Name) : r.GetString(d.Namespace) + "." + r.GetString(d.Name);
    }
    public string GetTypeFromSpecification(MetadataReader r, object c, TypeSpecificationHandle h, byte k) => r.GetTypeSpecification(h).DecodeSignature(this, c);
    public string Name(MetadataReader r, EntityHandle h) {
        if (h.IsNil) return "nil";
        switch (h.Kind) {
            case HandleKind.TypeDefinition: return GetTypeFromDefinition(r, (TypeDefinitionHandle)h, 0);
            case HandleKind.TypeReference: return GetTypeFromReference(r, (TypeReferenceHandle)h, 0);
            case HandleKind.TypeSpecification: return GetTypeFromSpecification(r, null, (TypeSpecificationHandle)h, 0);
            default: throw new InvalidDataException("Unsupported public metadata type handle: " + h.Kind);
        }
    }
    public static string Format(MethodSignature<string> s) => s.Header.RawValue + ":" + s.GenericParameterCount + ":" + s.RequiredParameterCount + ":" + s.ReturnType + "(" + string.Join(",", s.ParameterTypes) + ")";
}
public static class WinmdContract {
    static string Attribute(MetadataReader r, CustomAttributeHandle h, SignatureNames p) {
        var a = r.GetCustomAttribute(h); string type;
        if (a.Constructor.Kind == HandleKind.MemberReference) type = p.Name(r, r.GetMemberReference((MemberReferenceHandle)a.Constructor).Parent);
        else if (a.Constructor.Kind == HandleKind.MethodDefinition) type = p.Name(r, r.GetMethodDefinition((MethodDefinitionHandle)a.Constructor).GetDeclaringType());
        else throw new InvalidDataException("Unsupported custom attribute constructor.");
        return type + ":" + BitConverter.ToString(r.GetBlobBytes(a.Value));
    }
    public static string Read(string path) {
        using (var stream = File.OpenRead(path)) using (var pe = new PEReader(stream)) {
            var r = pe.GetMetadataReader(MetadataReaderOptions.None); var p = new SignatureNames(); var rows = new List<string>();
            foreach (var h in r.TypeDefinitions) {
                var t = r.GetTypeDefinition(h);
                if (r.GetString(t.Name) == "<Module>") continue;
                // C++/CX's generated default/factory interfaces are nonpublic,
                // but their IIDs and vtable slots are part of the shell ABI.
                var name = p.Name(r, h); rows.Add("type " + name + " " + (int)t.Attributes + " base=" + p.Name(r, t.BaseType));
                foreach (var a in t.GetCustomAttributes()) rows.Add(name + " attribute " + Attribute(r, a, p));
                foreach (var i in t.GetInterfaceImplementations()) rows.Add(name + " implements " + p.Name(r, r.GetInterfaceImplementation(i).Interface));
                int slot = 0;
                foreach (var mh in t.GetMethods()) {
                    var m = r.GetMethodDefinition(mh);
                    if ((m.Attributes & MethodAttributes.MemberAccessMask) != MethodAttributes.Public) continue;
                    string key = name + " method-slot=" + slot++ + " " + r.GetString(m.Name);
                    rows.Add(key + " " + (int)m.Attributes + " " + SignatureNames.Format(m.DecodeSignature(p, null)));
                    foreach (var a in m.GetCustomAttributes()) rows.Add(key + " attribute " + Attribute(r, a, p));
                    foreach (var ph in m.GetParameters()) {
                        var param = r.GetParameter(ph);
                        rows.Add(key + " param=" + param.SequenceNumber + " " + (int)param.Attributes + " " + r.GetString(param.Name));
                    }
                }
                foreach (var ph in t.GetProperties()) {
                    var prop = r.GetPropertyDefinition(ph);
                    rows.Add(name + " property " + r.GetString(prop.Name) + " " + (int)prop.Attributes + " " + SignatureNames.Format(prop.DecodeSignature(p, null)));
                }
                foreach (var eh in t.GetEvents()) {
                    var e = r.GetEventDefinition(eh);
                    rows.Add(name + " event " + r.GetString(e.Name) + " " + (int)e.Attributes + " " + p.Name(r, e.Type));
                }
                foreach (var fh in t.GetFields()) {
                    var f = r.GetFieldDefinition(fh); var constant = f.GetDefaultValue();
                    rows.Add(name + " field " + r.GetString(f.Name) + " " + (int)f.Attributes + " " + f.DecodeSignature(p, null) + " " + (constant.IsNil ? "" : BitConverter.ToString(r.GetBlobBytes(r.GetConstant(constant).Value))));
                }
            }
            rows.Sort(StringComparer.Ordinal); return string.Join("\n", rows) + "\n";
        }
    }
}}
'@
    # Use pwsh's reference assemblies. Runtime TPA assemblies are implementation
    # binaries and can produce unresolved type-forwarders in Add-Type compilation.
    Add-Type -TypeDefinition $metadataCode
}

$bridgeDll = Join-Path $bridge 'Win32Bridge.Runtime.dll'
$bridgeWinmd = Join-Path $bridge 'Win32Bridge.winmd'
foreach ($path in @($bridgeDll, $bridgeWinmd)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing bridge component: $path" }
}
Test-BridgeDll $bridgeDll
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedSourceSha256 -and $sourceHash -cne $ExpectedSourceSha256.ToLowerInvariant()) { throw 'SourcePackage SHA-256 does not match ExpectedSourceSha256.' }
$verifiedFiles = Test-SourceBlockMap $source
$sdk = Find-SdkTools
$makeappx = Join-Path $sdk 'makeappx.exe'
$signtool = Join-Path $sdk 'signtool.exe'
$work = Join-Path $output ('.desktop-repack-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'payload'
$certificate = $null
$temporaryRoot = $false
$completed = $false
try {
    New-Item -ItemType Directory -Path $work | Out-Null
    Invoke-SdkTool $makeappx @('unpack', '/p', $source, '/d', $payload, '/o') (Join-Path $output 'makeappx-unpack.log')
    $manifestPath = Join-Path $payload 'AppxManifest.xml'
    $manifestStream = [IO.File]::OpenRead($manifestPath)
    try { $manifest = Read-SafeXml $manifestStream } finally { $manifestStream.Dispose() }
    $identity = $manifest.SelectSingleNode("/*[local-name()='Package']/*[local-name()='Identity']")
    if ($null -eq $identity -or $identity.GetAttribute('Name') -cne 'Microsoft.MicrosoftEdge.CoreShell' -or
        $identity.GetAttribute('ProcessorArchitecture') -cne 'x64') { throw 'Source must be the x64 DesktopMode CoreShell package.' }
    $oldVersion = [version]$identity.GetAttribute('Version')
    if ($newVersion -le $oldVersion) { throw "Version $Version must be greater than source version $oldVersion." }
    $publisher = $identity.GetAttribute('Publisher')
    if ([string]::IsNullOrWhiteSpace($publisher)) { throw 'Source manifest publisher is missing.' }
    $sourceDll = Join-Path $payload 'Win32Bridge.Runtime.dll'
    $sourceWinmd = Join-Path $payload 'Win32Bridge.winmd'
    foreach ($path in @($sourceDll, $sourceWinmd)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Source package component is missing: $path" } }
    $registration = $manifest.SelectNodes("//*[local-name()='InProcessServer']") | Where-Object {
        $registeredPath = $_.SelectSingleNode("*[local-name()='Path']")
        $null -ne $registeredPath -and $registeredPath.InnerText -ceq 'Win32Bridge.Runtime.dll'
    }
    $classes = @($registration.SelectNodes("*[local-name()='ActivatableClass']") | ForEach-Object { $_.GetAttribute('ActivatableClassId') })
    foreach ($name in @('Win32Bridge.RuntimeSession', 'Win32Bridge.RuntimeHost')) {
        if ($name -cnotin $classes) { throw "Source manifest does not register $name in the expected runtime DLL." }
    }
    $originalContract = [DesktopBridgeRepack.WinmdContract]::Read($sourceWinmd)
    $replacementContract = [DesktopBridgeRepack.WinmdContract]::Read($bridgeWinmd)
    foreach ($name in @('Win32Bridge.RuntimeHost', 'Win32Bridge.RuntimeSession')) {
        if (-not $originalContract.Contains("type $name ", [StringComparison]::Ordinal)) { throw "Source metadata is missing $name." }
    }
    $originalContract | Set-Content -Encoding utf8 (Join-Path $output 'source-winrt-contract.txt')
    $replacementContract | Set-Content -Encoding utf8 (Join-Path $output 'replacement-winrt-contract.txt')
    if ($originalContract -cne $replacementContract) { throw 'Public WinRT ABI changed. The existing compiled shell cannot be reused; compare the contract files.' }
    $originalDllHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $bridgeDllHash = (Get-FileHash -LiteralPath $bridgeDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $bridgeWinmdHash = (Get-FileHash -LiteralPath $bridgeWinmd -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $bridgeDll -Destination $sourceDll -Force
    Copy-Item -LiteralPath $bridgeWinmd -Destination $sourceWinmd -Force
    if ((Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash.ToLowerInvariant() -cne $bridgeDllHash -or
        (Get-FileHash -LiteralPath $sourceWinmd -Algorithm SHA256).Hash.ToLowerInvariant() -cne $bridgeWinmdHash) { throw 'Copied bridge component hash mismatch.' }
    $identity.SetAttribute('Version', $Version)
    $xmlSettings = [Xml.XmlWriterSettings]::new()
    $xmlSettings.Encoding = [Text.UTF8Encoding]::new($false)
    $writer = [Xml.XmlWriter]::Create($manifestPath, $xmlSettings)
    try { $manifest.Save($writer) } finally { $writer.Dispose() }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $output 'AppxManifest.xml')

    $notices = Join-Path $payload 'ThirdParty\CommunityWin32Bridge'
    New-Item -ItemType Directory -Force -Path $notices | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot 'external\DesktopMode_uwp\LICENSE') -Destination (Join-Path $notices 'DesktopMode-MIT-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $notices 'XboxDesktopBridge-MIT-LICENSE.txt')
    foreach ($name in @('LICENSE', 'LICENSE_INFO', 'XCDM-INTEGRATION.md')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "third_party\libx86emu\$name") -Destination (Join-Path $notices "libx86emu-$name.txt")
    }
    @"
Community developer test repack of DesktopMode_uwp.
Only Win32Bridge.Runtime.dll, Win32Bridge.winmd, the package version, and these notices were changed.
The existing shell and Gecko payload are retained without rebuilding.
The manifest publisher name is retained for package identity compatibility.
The new self-signed certificate is a community developer test certificate, not an official Microsoft signature.
Source package SHA-256: $sourceHash
Replacement bridge DLL SHA-256: $bridgeDllHash
"@ | Set-Content -Encoding utf8 (Join-Path $notices 'REPACK-NOTICE.txt')
    foreach ($name in @('AppxSignature.p7x', 'AppxBlockMap.xml', '[Content_Types].xml', 'AppxMetadata\CodeIntegrity.cat')) {
        $obsolete = Join-Path $payload $name
        if (Test-Path -LiteralPath $obsolete) { Remove-Item -LiteralPath $obsolete -Force }
    }
    Invoke-SdkTool $makeappx @('pack', '/d', $payload, '/p', $package, '/o') (Join-Path $output 'makeappx-pack.log')
    $certificate = New-SelfSignedCertificate -Type Custom -Subject $publisher `
        -FriendlyName 'CommunityDeveloperTest DesktopMode Win32Bridge temporary certificate' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -KeyExportPolicy NonExportable `
        -HashAlgorithm SHA256 -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(12) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    Export-Certificate -Cert $certificate -FilePath $certificateFile | Out-Null
    Invoke-SdkTool $signtool @('sign', '/fd', 'SHA256', '/sha1', $certificate.Thumbprint, '/s', 'My',
        '/d', 'Community Developer Test - DesktopMode Win32Bridge', $package) (Join-Path $output 'signtool-sign.log')
    # Scope trust to this verification and remove only this newly generated
    # certificate afterward. No certificate/private key is left installed.
    Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\CurrentUser\Root' | Out-Null
    $temporaryRoot = $true
    Invoke-SdkTool $signtool @('verify', '/pa', '/all', '/v', $package) (Join-Path $output 'signtool-verify.log')
    $verifiedOutputFiles = Test-SourceBlockMap $package
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash) { throw 'Source package changed during repackaging.' }
    [ordered]@{
        PackageType = 'COMMUNITY-DEVELOPER-TEST'
        SourcePackage = $source
        SourceSha256 = $sourceHash
        SourceExpectedSha256 = $ExpectedSourceSha256
        SourceBlockMapVerifiedFiles = $verifiedFiles
        SourceVersion = $oldVersion.ToString(4)
        PackageIdentity = $identity.GetAttribute('Name')
        Publisher = $publisher
        Version = $Version
        Architecture = 'x64'
        OriginalBridgeSha256 = $originalDllHash
        BridgeSha256 = $bridgeDllHash
        BridgeWinmdSha256 = $bridgeWinmdHash
        PublicWinRTContractSha256 = (Get-FileHash -LiteralPath (Join-Path $output 'replacement-winrt-contract.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
        PublicWinRTContract = 'Unchanged: public types, interface IIDs, method order/signatures, properties, events, fields'
        Package = $package
        PackageSha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
        PackageBlockMapVerifiedFiles = $verifiedOutputFiles
        CertificateSubject = $certificate.Subject
        CertificateThumbprint = $certificate.Thumbprint
        PublicCertificate = $certificateFile
        SignatureVerification = 'SignTool /pa /all; temporary CurrentUser Root trust removed in finally'
        WindowsSdkTools = $sdk
        XboxHardwareTested = $false
        Note = 'Community self-signed test build. Package validation does not establish game compatibility.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $output 'repack-info.json')
    $completed = $true
    Write-Host "Created community developer test package: $package"
    Write-Host "Public certificate: $certificateFile"
} finally {
    if ($null -ne $certificate) {
        try {
            if ($temporaryRoot -or (Test-Path "Cert:\CurrentUser\Root\$($certificate.Thumbprint)")) {
                Remove-Item -LiteralPath "Cert:\CurrentUser\Root\$($certificate.Thumbprint)" -Force -ErrorAction Stop
            }
        } finally {
            Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -Force -ErrorAction Stop
        }
    }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    if (-not $completed -and (Test-Path -LiteralPath $package)) { Remove-Item -LiteralPath $package -Force }
}
