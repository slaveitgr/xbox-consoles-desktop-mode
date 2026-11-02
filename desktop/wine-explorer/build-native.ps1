<#
.SYNOPSIS
Compile the actual pinned Wine Explorer file-window source as a native PE64 guest.
.DESCRIPTION
Uses desktop VS2022/Windows SDK, not UWP prerequisites. No default CRT, Wine Unix
loader, emulator, or replacement ExplorerBrowser is linked. The actual shell
provider and desktop session remain separate work. This never deploys or runs
the Wine executable; host-only adapter contract checks are executed.
#>
[CmdletBinding()]
param([string]$WindowsSdkVersion)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$toolchain = & (Join-Path $PSScriptRoot 'find-desktop-toolchain.ps1') -WindowsSdkVersion $WindowsSdkVersion
$output = Join-Path $PSScriptRoot 'build\native'
New-Item -ItemType Directory -Force -Path $output | Out-Null
& python (Join-Path $PSScriptRoot 'prepare_sources.py')
if ($LASTEXITCODE -ne 0) { throw 'Pinned source preparation failed.' }
& python (Join-Path $PSScriptRoot 'native\prepare_native.py')
if ($LASTEXITCODE -ne 0) { throw 'Native Wine source adaptation failed.' }
& python -m unittest discover -s (Join-Path $PSScriptRoot 'native\tests') -p test_source_tools.py -v
if ($LASTEXITCODE -ne 0) { throw 'Native source/import build gate tests failed.' }

$testBuild = Join-Path $output 'host-tests'
& cmake -S (Join-Path $PSScriptRoot 'native\tests') -B $testBuild -A x64 -T v143 "-DCMAKE_SYSTEM_VERSION=$($toolchain.WindowsSdkVersion)"
if ($LASTEXITCODE -ne 0) { throw 'Host adapter check configuration failed.' }
& cmake --build $testBuild --config Release
if ($LASTEXITCODE -ne 0) { throw 'Host adapter checks did not compile.' }
& ctest --test-dir $testBuild -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Host adapter contract checks failed.' }

& $toolchain.MSBuild (Join-Path $PSScriptRoot 'native\WineExplorerNative.vcxproj') `
    /t:Build /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64 `
    "/p:WindowsTargetPlatformVersion=$($toolchain.WindowsSdkVersion)" `
    "/bl:$(Join-Path $output 'build.binlog')" `
    "/flp:logfile=$(Join-Path $output 'build.log');verbosity=normal"
if ($LASTEXITCODE -ne 0) { throw "Native Wine Explorer source build failed: $LASTEXITCODE" }
$executable = Join-Path $output 'MihawkWineExplorer.exe'
if (-not (Test-Path $executable)) { throw 'The native source build produced no executable.' }
$dumpbinReport = Join-Path $output 'dumpbin.txt'
$dump = & $toolchain.Dumpbin /headers /imports $executable
if ($LASTEXITCODE -ne 0) { throw 'dumpbin could not inspect the native source build.' }
$dump | Set-Content -LiteralPath $dumpbinReport -Encoding utf8
& python (Join-Path $PSScriptRoot 'native\validate_native.py') $executable $dumpbinReport --output (Join-Path $output 'native-imports.json')
if ($LASTEXITCODE -ne 0) { throw 'Native PE64/import validation failed.' }
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'staged\wine\COPYING.LIB') (Join-Path $licenses 'Wine-COPYING.LIB') -Force
Copy-Item (Join-Path $PSScriptRoot 'README.md') (Join-Path $licenses 'SOURCE-AND-SCOPE.md') -Force
$corresponding = Join-Path $output 'source\corresponding\desktop\wine-explorer'
New-Item -ItemType Directory -Force -Path $corresponding | Out-Null
foreach ($file in @('README.md', 'source-lock.json', 'prepare_sources.py', 'audit_imports.py',
        'build-native.ps1', 'find-desktop-toolchain.ps1', 'build-probe.ps1',
        'ExplorerContractProbe.c', 'ExplorerContractProbe.vcxproj')) {
    Copy-Item (Join-Path $PSScriptRoot $file) (Join-Path $corresponding $file) -Force
}
foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'native') -Recurse -File | Where-Object Extension -ne '.pyc') {
    $relative = [IO.Path]::GetRelativePath($PSScriptRoot, $file.FullName)
    $target = Join-Path $corresponding $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item $file.FullName $target -Force
}
$stagedSource = Join-Path $corresponding 'staged'
New-Item -ItemType Directory -Force -Path $stagedSource | Out-Null
foreach ($upstream in @('wine', 'mihawk')) {
    Copy-Item (Join-Path $PSScriptRoot "staged\$upstream") $stagedSource -Recurse -Force
}
Copy-Item (Join-Path $PSScriptRoot 'staged\provenance.json') $stagedSource -Force
$head = & git -C $PSScriptRoot rev-parse HEAD
[ordered]@{
    schema = 1
    source_commit = "$head".Trim()
    windows_sdk = $toolchain.WindowsSdkVersion
    executable = 'MihawkWineExplorer.exe'
    native_pe64_imports_validated = $true
    host_adapter_contract_checks_passed = $true
    windows_execution_tested = $false
    physical_xbox_tested = $false
    wine_shell_provider_included = $false
    wine_desktop_session_included = $false
} | ConvertTo-Json | Set-Content (Join-Path $output 'native-build-info.json') -Encoding utf8
Write-Host "Built actual Wine Explorer PE64 file-window source: $executable"
Write-Host 'No Wine executable was run. Wine shell provider/desktop session are not included.'
