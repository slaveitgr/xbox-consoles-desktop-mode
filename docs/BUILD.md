# Build guide

## Two separate build products

- **RuntimeDiagnostics** is our small, independent UWP companion. It tests the
  platform without requiring the upstream shell, Gecko or commercial binaries.
- **DesktopMode UWP** is the pinned upstream desktop. A Win32Bridge DLL build is
  useful validation of one component; it does not produce the full desktop.

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
.\tools\build-uwp.ps1 -Configuration Release -Sign
```

`-Sign` creates a temporary development certificate whose subject matches our
own `CN=SlaveIT` publisher. It exports the public `.cer`, signs the package and
removes the certificate/private key from the store afterwards. It never commits
a private key. Each invocation creates a new test certificate; for repeated
local deployment, remove the previous test app if certificate trust conflicts.

Outputs are under `artifacts/diagnostics/packages/Release/test-signed/` with
logs under `artifacts/diagnostics/logs/`. Without `-Sign`, the result is unsigned
and must be signed before ordinary installation. The package metadata records
the toolchain, certificate thumbprint and that Xbox hardware has not been tested.

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
without packaging/signing. Downstream desktop package identity, assets and
content cleanup are still separate work; the companion is the only package
produced by our packaging pipeline today.

The exact engine host instructions are in
[`external/DesktopMode_uwp/gecko-source/GeckoShellHost/README.md`](../external/DesktopMode_uwp/gecko-source/GeckoShellHost/README.md).
The external engine is not silently replaced with an incompatible stock Firefox
download and missing input is reported as a build prerequisite, not a pass.

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
