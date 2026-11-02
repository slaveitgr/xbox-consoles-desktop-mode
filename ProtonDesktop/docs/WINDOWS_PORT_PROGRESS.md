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
not a source-only estimate. At that attempt no successful UWP core executable or
import audit existed; the later successful link is recorded below. The artifact ZIP digest was verified as
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
Debian metadata and source provenance. At this initial metadata milestone no
package/rootfs binaries were fetched or assembled; a subsequent local assembly
is recorded below. The boot contract has a separate two-base-layer prefix-initialization
mode; the existing desktop mode still requires a prepared third layer. Its 63
portable assertions pass both locally and on desktop Windows. A request description does not verify a real
empty app-owned overlay or execute Wine. Prefix completion also needs an explicit
persistent-wineserver shutdown policy and bounded waits, then validation of
registry files, directories and drive links before declaring it prepared.

## Successful UWP diagnostic link

Source `49dd9d66ac9a970ee949a2802fcbc32374f3915c`,
[run 41482983407](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41482983407):

- The actual guest-x64 diagnostic linked under `WindowsStore`, genuine
  `WINAPI_FAMILY_APP` headers and the Store C++ runtime.
- Real socket-pair wakeup, current-thread scheduling and string-search services
  resolved required functionality. The initial profile explicitly disables
  unsupported audio and CPU telemetry; it rejects host-code writes.
- Windows service checks passed: boot 63, strings 149, scheduling 91,
  socket pair 79, memory 49, host services 47 and profile 30.
- The basic import guard passed and `_APP` C++ runtime DLLs were selected.
  This is not a complete UWP API certification or full Wine-kernel closure.
  In particular, retained CRT environment functions still require qualification
  or replacement before a complete session can be accepted.

Verified artifact SHA-256:
`d83f8e9dcefb348726a7004a4fff4c9f4f35d3d634683d88b725ec23f9b64c5b`.
Executable SHA-256:
`72ada15653ea7eff2b87bf186aaa55200c513c282712bfbdc300174187649e11`.

The subsequent opt-in CoreApplication probe compiled and linked the actual core
into `ProtonDesktop.exe` in
[run 41488079728](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41488079728).
Packaging failed at `APPX1673` because the manifest lacked `PhoneIdentity`.
The manifest has been corrected; packaging validation is separate from linking,
and neither proves app activation. Default host and probe outputs remain separate.

The corrected probe **compiled, linked and packaged successfully** from
`ce43dcba3a8d1cf8fd4eff70140682e790def88b` in
[run 41490787752](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41490787752).
The unsigned `ProtonDesktop_0.1.0.0_x64.msix` contains the real interpreter and
both self-authored ELF controls. Package SHA-256:
`4e102c5c53a312934e983a1147d766cdd50a87c1c677aa9b1abf7e2df63f0e6a`.
Artifact SHA-256:
`73efe7978d404a55131967140ca3925b357bc427c678e0515009f6dce1a066ff`.
At this CI packaging stage the package was not signed, installed or activated. The independent software
SDL job in the same workflow failed its nested CMake compiler-link probe;
the successful package does not turn that separate failure into a pass.

## Filesystem and software pixels after the link

125 hash-verified Debian packages were assembled locally into two deterministic
ZIP candidates. The payload contains 5,941 entries and 107 ELF files. No guest
was executed and no Wine prefix was created. Private Wine library search order
and license-notice gaps remain unresolved; neither artifact is qualified for
boot or redistribution. Exact hashes and RAM-only locations are recorded in
`rootfs/README.md` and local ignored assembly evidence.

The custom SDL software video driver passed 300 Linux checks, including real
texture rendering and pixel copying into a host-owned buffer, under ASan/UBSan.
Its Windows APP-family compilation and execution are a separate qualification
step. No GPU/CoreWindow presentation, Wine window or Xbox pixels are proven by
that software test.

The old Boxedwine-UWP `uwp-compat` branch was re-reviewed: it already has genuine
AppContainer, SDL/WinRT, D3D11, input and storage implementations. The initial
review missed that branch. Those are reusable references for the remaining
integration; guest-x64 support does not inherently require a new renderer.

## Next proof

The real-core probe has since been locally developer-signed, installed and
activated on **Xbox One X** (SystemOS `26100.10448`, app `0.1.0.0`). The actual
app-owned JSON retrieved over verified Device Portal reports **907 passed,
0 failed**, positive ELF exit **0** after 20 instructions, and negative ELF
exit **9** after 7 instructions. All three phases returned with complete logs;
the user photo confirms the displayed result. Separate phase logs were not
downloaded. This is CPU/ELF execution evidence, not sustained UI stability,
rootfs/Wine execution or Explorer desktop qualification.

The software SDL driver also passed **300 actual pixel checks on desktop
Windows with APP headers and Store CRT**, source
`a7316b0fb34e2ffe2b844c28e98835a0f6424d1b`, run `41492970294`.
It does not qualify GPU, CoreWindow or Wine pixels.

1. Build the new native SDL/WinRT Wine application and retain the complete
   session/kernel/VFS graph in a real APP link.
2. Exercise the Wine lifecycle using the existing pinned kernel/ZIP/VFS, with
   explicit load failure and owned guest-worker completion. Qualify the
   filesystem and display a real Wine application before
   attempting Explorer desktop and applying the Windows 11-inspired theme.
