# Windows/UWP port qualification

Target: Xbox One X. Source pin remains
`58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4`. This document separates source
implementation, desktop Windows execution, UWP compilation and physical Xbox
results. None implies the next one.

## Current implementation

- The real CPU64/kernel/ELF source has a Windows desktop clang-cl target.
  SDL2 is compiled from the same pin, with a disposable CMake flag overlay.
  The official compiler-rt library supplies the compiler's 128-bit helpers.
- A portable self-authored ELF64 generator removes the assembler dependency
  for Windows CI. Its positive and deliberately failing payloads have both
  been run through the existing actual Linux interpreter.
- The UWP host implements deferred resize/DPI handling, device recreation on
  removal/reset and D2D target loss, suspend GPU trimming and resume validation.
  Those lifecycle paths still require actual device testing.
- The memory adapter implements the real upstream Platform memory signatures
  with `VirtualAllocFromApp`, `VirtualProtectFromApp` and `VirtualFree`, granting
  no executable host memory. CPU64's existing software page allocator is not
  replaced. The adapter cannot be linked together with the duplicate desktop
  Platform definitions.

The app's backend factory is still unavailable. No call starts a Wine process.
This is not a working Explorer desktop or a game compatibility result.

## Windows CI

Development checks use the isolated `codex/protondesktop` branch of
`slaveitgr/xbox-consoles-desktop-mode`, under `ProtonDesktop/`. The native XWindows
OS development branch is not modified. No ProtonDesktop package is deployed.

First attempt:
[`41466472013`](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41466472013),
source `2ebd0ddc766624921144e7ba5597c7612414beaf`.

- Windows portable suites passed (17 contract assertions and seven Python fixtures).
- clang-cl's actual 128-bit arithmetic probe linked with compiler-rt.
- SDL compilation failed because its old CMake treated clang-cl as the GNU
  driver and passed `-idirafter`. A one-line build-local overlay now selects
  the existing MSVC include-directory path.
- UWP host compilation did not start: the SDK installer rejected persistent
  MSBuild nodes left by the preceding tests. Setup now runs before any MSBuild
  and CI disables node reuse.
- The standalone FromApp memory library compiled, but compiling the upstream
  Platform signatures stopped on headers omitted by CI's sparse checkout.
  The source/platform header trees are now included.

Second source snapshot `d65216910a96844dc71ef0b90a9b9d2d7e961797` compiled
the actual upstream memory adapter declarations and passed **49 Windows memory
checks**. The host compiler exposed two C++/WinRT call-signature errors, fixed
before the next host build. SDL then needed its legacy clang-cl CPU probe
declarations and scalar conversion configuration corrected.

Successful host source `217a2791bfed70d0e224eb5c233816a402c8c3c6`,
[run 41469175033](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41469175033):

- UWP x64 host compiled and linked with `/APPCONTAINER`.
- The two portable suites passed on Windows.
- Both adapter suites passed: **49 memory checks and 47 host-service checks**.
- Actual upstream Platform signatures compiled under `WINAPI_FAMILY_APP`.
- Tests ran in desktop Windows test executables. They do not qualify an
  AppContainer runtime session, Xbox folder permissions or Wine execution.

## Actual Windows interpreter execution

Passed [run 41472941389](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41472941389),
source `b0115e6dc53380a06c38b20284cd1d004896b47a`:

- The actual guest-x64 core and bundled SDL compiled and linked on Windows x64
  with clang-cl 19.1.5, real CRT headers/function probes and compiler-rt.
- **907 CPU64 checks passed, zero failed**.
- Our generated static ELF64 produced its expected output and guest exit 0.
  The deliberately wrong arithmetic control produced no marker and guest exit 9.
- The normal four CTest suites passed, including 49 memory and 47 host-service
  checks under the UWP API family in Windows test executables.

Verified result: `artifacts/windows-core-proof-b0115e6/` locally. The CI artifact
ZIP digest is `5eaa5533bf6c23d89fa697f1f69a8854327c46ffd703170f13ec8f62ab4d53a6`.
The interpreter executable digest is
`1cb9378ddd9fabaab565de58d3457f6e8882cb557c9126b6254493bd018c0096`.

This is a desktop Windows interpreter result. The separate WindowsStore target
in `tools/uwp-core/` is being compiled with genuine `WINAPI_FAMILY_APP` headers,
WindowsApp imports and the implemented platform subset. Missing required
services must fail compilation/linkage. A link diagnostic is not package
activation or a Wine result. The app backend remains unavailable.

The first WindowsStore attempt,
[run 41474540731](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41474540731),
compiled the host again and reached the actual upstream source build. It failed
at SDL dynapi's desktop message-box / WinRT declaration path and the upstream
desktop launcher's DPI/current-directory calls. These are compile failures,
not a successful UWP core link. The next diagnostic uses SDL's own WinRT build
mode and bypasses only the unused desktop launcher, retaining the actual
interpreter and ELF test entry points.

The second attempt,
[run 41477416523](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41477416523),
source `3fd0fd1c56da03af14ab3fac5de2116c4a1cabe6`, reached the final executable
link with **360 object-build steps and zero compilation errors**. The link failed
with these 11 unresolved definitions:

| Definitions | Referencing functionality |
| --- | --- |
| `nativeSocketPair` | Native socket polling/wakeup thread |
| `setCpuAffinityForThread`, `setCurrentThreadPriorityHigh` | Guest thread/process scheduling |
| `getCpuFreqMHz`, `getCpuCurScalingFreqMHz`, `getCpuMaxScalingFreqMHz`, `procStat` | Guest proc/sys CPU information |
| `getPixelFormats` | Kernel display-format initialization |
| `writeCodeToMemory` | Shared native code-heap branch; JIT is disabled and must remain rejected |
| `platform_strcasestr` | Case-insensitive string search |
| `initWindowsAudio` | Windows-specific audio initialization, outside the intended initial audio-disabled profile |

`Platform::` prefixes are omitted in the table. This is an actual linker report,
not a source-only estimate. No successful UWP core executable or import audit
exists. The artifact ZIP digest was verified as
`2c09a426c20eddf0dcafbc1e135f5f9f1c00ae15b8754ba72a6dacf235fb6b69`;
local extracted logs are in `artifacts/uwp-core-attempt2/`.

The same run again compiled the native UWP host. Its four CTest suites passed:
**63 boot assertions, seven Python fixtures, 49 memory checks and 47 host-service
checks**. These ran as desktop Windows test executables. Neither the UWP host
nor the interpreter was activated in AppContainer or on Xbox.

Even a future successful diagnostic link qualifies its retained call graph
only; function/data dead stripping can remove other guest kernel paths. The
full Wine process/VFS/socket graph and software pixel presenter must be linked
and qualified separately. See the closure boundary in `tools/uwp-core/README.md`.

## Wine filesystem and first boot

`rootfs/` now records Wine 8.0~repack-4 and glibc 2.36-9+deb12u14 with signed
Debian metadata and source provenance. No package/rootfs binaries were fetched
or assembled. The boot contract has a separate two-base-layer prefix-initialization
mode; the existing desktop mode still requires a prepared third layer. Its 63
portable assertions pass both locally and on desktop Windows. A request description does not verify a real
empty app-owned overlay or execute Wine. Prefix completion also needs an explicit
persistent-wineserver shutdown policy and bounded waits, then validation of
registry files, directories and drive links before declaring it prepared.

## Next proof

1. Build the interpreter under the UWP API partition, porting the remaining
   platform services and presentation path, then repeat ELF64 checks on Xbox.
2. Supply a pinned Wine64 rootfs and display a real Wine application before
   attempting Explorer desktop and applying the Windows 11-inspired theme.
