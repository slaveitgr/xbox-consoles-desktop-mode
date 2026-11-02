# Verified baseline

## Packaged Win32Bridge and game startup, version 0.7

Source commit: `4d4f2bcebaf465ea46bfee2864a2462004efb7ba`;
[Actions run 41378042706](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41378042706).
All seven Linux checks and seven Windows checks passed. CI produced the test-signed UWP
companion, the patched Win32Bridge, and desktop repack **1.0.46.2**. The repack
passed WinRT ABI comparison, SignTool signature verification and blockmap
validation of all **8,858 files**. The desktop update then installed on the same
One X with deployment code 0. Its launch returned HTTP 200 and a new
`DesktopMode.exe` process was observed. The console registered package version
**1.0.46.2**. Screenshot capture for this shell remains unavailable; its new
visual behavior and PE32 file-open interaction still need direct observation.

Physical Xbox One X, OS **10.0.26100.10448**, diagnostics **0.7.0.0**:

- The app loaded the packaged `Win32Bridge.Runtime.dll`, ABI 1, without a direct
  runtime fallback. Its SHA-256 is
  `72498347213ce3589427d879bdf5607153e73f921ceb665571614d9d6a0378a9`.
- The embedded self-authored PE32 smoke test passed: 13 instructions and four
  typed host calls. Simultaneous native low-address memory reservations passed.
- The saved USB grant reopened `gta_sa.exe` automatically. Its startup trace
  executed 41 guest instructions and host gates from entry point `0x824570`,
  then stopped at the unimplemented `KERNEL32.dll!GetVersionExA` gate
  `0x7ff005c0`. All 218 imports were bound to gates; only four host APIs are
  implemented. Import binding does not mean those APIs are supported.

The earlier version 0.5 guest W+X-section stop and version 0.6
read-only-IAT loader stop are fixed. This trace did not reach a successful
`ExitProcess(0)`, gameplay or graphics. Separately, desktop repack **1.0.46.1**
was installed, launched and observed running on the console; this does not
establish game compatibility.

## USB metadata inspection, version 0.3

On 2026-11-02 (Europe/Athens), version **0.3.0.0** on the same One X and OS
successfully opened a USB folder through FolderPicker, read `gta_sa.exe` in
place, parsed its PE32/x86 metadata and saved `game-preflight.txt`. Both a
console screenshot and the report were retrieved through Device Portal.
The report identified 13 imported libraries / 218 imports, including delayed
Direct3D 9, DirectInput8 and DirectSound. The expected game data folders were
present. No game code was executed.

The WDP filesystem API requires `path=\LocalState` (including the leading
Windows separator). A query using plain `LocalState` addresses a different
path and does not establish that the application failed to save its report.

Build source: `29b33c6619fda513f4c19d8af1facc0043b75c27`;
[Actions run 41367529549](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41367529549)
passed Linux/Windows checks, UWP packaging and the pinned bridge build.

## Original native x64 baseline

Test date: **2026-11-02, Europe/Athens** (report timestamp 2026-11-01 22:42:50 UTC).

## Build and host checks

- Application source commit: `8e579891132a1411e70c1c1f95b4cfe62931bc7b`.
- [GitHub Actions run 41357703047](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41357703047): successful.
- Linux GCC host checks with AddressSanitizer/UndefinedBehaviorSanitizer: passed.
- Parser checks: 7,338 passed.
- Windows MSVC x64 host parser and actual execution probes: passed.
- UWP native host: compiled; unsigned and test-signed packages produced.
- Pinned upstream Win32Bridge: compiled to `Win32Bridge.Runtime.dll`.
- Package format produced by this SDK: MSIX, with the x64 VCLibs dependency.

## Physical console

| Item | Observed result |
| --- | --- |
| Hardware | Xbox One X |
| OS | 10.0.26100.10448 |
| Package | SlaveIT.XboxDesktopDiagnostics, 0.1.0.0, x64 |
| Device Portal deployment | Success, deployment code 0 |
| Application launch and rendering | Confirmed by console screenshot |
| Writable-memory to executable-memory probe | Passed; native x64 code returned 42; error code 0 |
| Minimal PE32+ x64 probe | Passed; mapped no-import fixture returned 42; error code 0 |
| Report persistence | `runtime-diagnostics.txt` saved and retrieved from app LocalState |

Console access used TLS with the console-supplied trust key and hostname
validation. No account credentials, account identifiers, network addresses or
private certificates are included in this public report.

## Limits of this result

The fixture is a tiny self-authored position-independent program with no imports,
CRT, TLS, relocations or graphics calls. This proves native code execution and
the constrained mapping contract inside our UWP application on this console.
It does not prove support for ordinary desktop apps, Steam, Direct3D games,
32-bit code, DRM, anti-cheat or arbitrary PE entry-point initialization.

## Upstream desktop baseline

On the same test date, the published upstream release
[21286](https://github.com/Pedro1234-code/DesktopMode_uwp/releases/tag/21286),
package version **1.0.46.0 x64**, was installed with all five supplied x64
framework dependencies. All six downloaded files matched their published
GitHub SHA-256 digests.

- No installed package had the upstream CoreShell identity before deployment;
  the installed Edge Stable package has a different identity.
- Device Portal deployment completed successfully, code 0.
- Device Portal accepted launch, HTTP 200; process enumeration then reported
  `DesktopMode.exe` running.
- The user confirmed that the desktop and taskbar appeared on the physical TV.
- Device Portal screenshot capture failed after this launch, so this visual
  confirmation is a user observation, not an independently captured screenshot.

This validates installation and initial shell display of the **upstream binary**.
It does not mean our repository independently built or customized that desktop.
See [`DESKTOP_BASELINE.md`](DESKTOP_BASELINE.md) for the exact package/dependencies.
The full source build still needs upstream's external Gecko inputs. Built-in app
interaction, upstream Win32 application compatibility, controller reconnect,
keyboard behavior and extended suspend/resume qualification remain outstanding.
