# Build guide

## Build products

- **RuntimeDiagnostics** is our small, independent UWP companion. It tests the
  platform without requiring the upstream shell, Gecko or commercial binaries.
- **DesktopMode UWP** is the pinned upstream desktop. A Win32Bridge DLL build is
  useful validation of one component; it does not produce the full desktop.
- The **desktop repack** replaces that component in the pinned upstream binary
  package after checking its WinRT ABI. It retains the existing shell and Gecko.

## Windows prerequisites

Use Visual Studio 2022 with C++ desktop tools and C++ Universal Windows Platform
tools (`Microsoft.VisualStudio.ComponentGroup.UWP.VC`), v143, and Windows SDK
10.0.22621.0 or newer including C++/WinRT headers and packaging/signing tools.
The project defaults to SDK 10.0.26100.0; the build script chooses an installed
compatible SDK automatically. Build x64, including for Xbox One.

The full upstream C# shell additionally requires the C# UWP/.NET Native build
tools, its NuGet dependencies, and the external Gecko inputs below.

## Diagnostics companion

From PowerShell on Windows:

```powershell
.\tools\build-uwp.ps1 -CheckOnly
git submodule update --init --recursive
.\tools\build-upstream.ps1 -Configuration Release -WithX86Backend
.\tools\build-uwp.ps1 -Configuration Release -Sign
```

The patched bridge build stages `app/BridgeRuntime/Win32Bridge.Runtime.dll`,
which version 0.6 and later companions load explicitly. Build it before the
companion. Its overlay is applied to a disposable copy, leaving the submodule
unchanged. Outputs are under `artifacts/upstream-bridge-x86/Release/`.

`-Sign` creates a temporary development certificate whose subject matches our
own `CN=SlaveIT` publisher. It exports the public `.cer`, signs the package and
removes the certificate/private key from the store afterwards. It never commits
a private key. Each invocation creates a new test certificate; for repeated
local deployment, remove the previous test app if certificate trust conflicts.

AppX/MSIX outputs are under `artifacts/diagnostics/packages/Release/test-signed/` with
logs under `artifacts/diagnostics/logs/`. Without `-Sign`, the result is unsigned
and must be signed before ordinary installation. The package metadata records
the toolchain, certificate thumbprint and that this build job itself does not
perform hardware testing. Physical test evidence is recorded separately in
`VALIDATION.md`.

On a Windows development PC, trust only the public test certificate you built
and install the package plus its generated x64 dependencies. For Xbox, use
Developer Mode and Device Portal and follow the signing/dependency requirements
of that development setup. No Microsoft publisher certificate is used by our
companion. Building/signing is not proof that installation or execution passed.

The IDE entry is `XboxDesktopMode.sln`. Packaging from the script is recommended
because it checks the toolchain and sets explicit package/signing properties.

## Upstream Win32Bridge component

```powershell
git submodule update --init --recursive
.\tools\build-upstream.ps1 -Configuration Release
```

This compiles `Win32Bridge.Runtime.dll` and metadata into
`artifacts/upstream-bridge/Release/`. It does not build/sign the desktop or
package upstream guest applications. It does not modify the pinned submodule.

## Full upstream desktop

The source checkout alone is insufficient. First obtain/build the compatible
patched Gecko engine specified by upstream, then set:

```powershell
$env:GECKO_W10M_SOURCE = 'C:\path\to\patched-firefox-source'
$env:GECKO_W10M_OBJ = 'C:\path\to\gecko-object-directory'
$env:GECKO_W10M_CLANG_CL = 'C:\Program Files\LLVM\bin\clang-cl.exe'
.\tools\build-upstream.ps1 -FullShell -Configuration Release
```

The wrapper validates the source directory, `mozilla-config.h`, `xul.dll`,
`application.ini` and compiler before invoking MSBuild. It deliberately compiles
without packaging/signing. An independently rebuilt complete shell is still
separate from the binary repack described below.

The exact engine host instructions are in
[`external/DesktopMode_uwp/gecko-source/GeckoShellHost/README.md`](../external/DesktopMode_uwp/gecko-source/GeckoShellHost/README.md).
The external engine is not silently replaced with an incompatible stock Firefox
download and missing input is reported as a build prerequisite, not a pass.

## Desktop repack with the patched bridge

After building with `-WithX86Backend`, run PowerShell 7 elevated on an ephemeral
Windows build machine:

```powershell
.\tools\repack-desktop.ps1 `
  -SourcePackage C:\input\DesktopMode_1.0.46.0_x64.msix `
  -BridgeDirectory artifacts\upstream-bridge-x86\Release `
  -OutputDirectory artifacts\patched-desktop `
  -Version 1.0.46.2 `
  -ExpectedSourceSha256 4691181e07f68f8d8c58e96ad44d4a01a9d2ccf1381669fd3ffa61d1e492beea
```

The helper validates block hashes and the public WinRT contract before replacing
the DLL/metadata and incrementing the version. It adds component licenses and
signs with a temporary community development certificate. The original manifest
publisher is retained for package identity compatibility; this is not an official
Microsoft signature. SignTool verification temporarily trusts only that new
certificate in LocalMachine Root, then removes it and the signing private key.
Only the public certificate is exported. The installed shell and Gecko binaries
are reused rather than compiled by this command.

## Host tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DXCDM_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizers apply to GCC/Clang host builds. Windows/MSVC CMake builds run the same
parser checks and require both execution probes to actually pass in the ordinary
desktop host process. Linux checks that native execution returns Unsupported.
Neither substitutes for the packaged UWP or physical Xbox tests.
