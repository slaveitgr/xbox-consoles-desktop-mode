# Verified baseline

## Certificate metadata and Steam installer trace, version 0.10

Source commit: `d271fc4c72b0e15d33a20e686fce3704e263676a`;
source tree: `9fb456acf00de4beb7da06522bbf5b9510773c69`;
[Actions run 41416818336](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41416818336).
All seven Linux ASan/UBSan suites and seven Windows suites passed, including
**323 PE32 loader and execution checks** and **56 bounded startup checks**.
Both CI jobs completed successfully. The fixture and 18 native
startup-target parser checks passed; read-only core review found no blocker.
Windows build, packaging and desktop repack checks passed. Desktop repack
**1.0.46.5** passed signature/ABI checks and validation of all **8,858 blockmap
files** in CI. Console deployment completed with code 0 and success true;
version **1.0.46.5** was registered, launch returned HTTP 200, and
`DesktopMode.exe` was observed under that exact package identity. A fresh
SteamSetup file-open trace through this desktop remains unobserved. The
physical 13-instruction result below is from diagnostics sharing the exact DLL.

Physical Xbox One X, diagnostics **0.10.0.0**:

- Deployment completed with code 0 and launch returned HTTP 200. The packaged
  bridge SHA-256 is
  `8255e7ac61c70f8120c7b5234b02c1540b67417db2af50911f827961e1a1b1e5`.
- The latest `x86-runtime.txt` confirms the original four-call smoke test passed
  in 13 instructions. The native simultaneous low-address reservation probe
  passed and released its reservations.
- The default USB GTA trace retained its 85-instruction HeapCreate stop.
- An invalid target override, `../SteamSetup.exe`, reported `Runner invoked: no`
  and `File: No target selected`; it did not fall back to GTA.
- A valid `SteamSetup.exe` override reopened the saved USB grant and read the
  2,380,800-byte installer in place; it matched the official inspected binary.
  It executed **13 guest instructions
  and host gates**, stopping at `KERNEL32.dll!SetErrorMode`, gate `0x7ff00110`.
  Its report identified one certificate record at file offset `0x242698`, size
  11,624 bytes, explicitly unmapped and unverified. No ExitProcess was reached.

The override was removed after testing, restoring the default target. This
demonstrates bounded installer startup on Xbox, without Steam installation or
client UI. No new graphics or gameplay result was observed.

StartupTrace now accepts structurally valid SECURITY directory 4 certificate
tables as bounded file metadata. The address is a file offset, not a guest RVA;
8-byte alignment, record extents, revision/type, zero padding, record limits and
disjoint header/raw-section ranges are checked. Only revision 0x0200 and type
0x0002 are accepted. Certificate bytes are never mapped, and Authenticode
signature/trust is not verified. The strict Fixture profile still rejects this
directory; TLS, relocations and CLR remain unsupported with precise diagnostics.

On the development host, the official `SteamSetup.exe` also maps and executes
**13 guest instructions and host gates** through the bounded interpreter,
stopping at unimplemented `KERNEL32.dll!SetErrorMode`, gate `0x7ff00110`.
No ExitProcess call was reached. The host result is recorded separately from
the matching console result above.

The diagnostic app also accepts a basename override in
`LocalState/startup-target.txt` for an executable in the saved folder. An absent
override retains `gta_sa.exe`; an invalid override stops without falling back
to another executable. Both override paths were verified on the console.

## GetModuleHandleA startup progress, version 0.9

Source commit: `d3b03db19eb4a12c8697bb5966ea2b22c82cb4e9`;
source tree: `ba8f350287b0075e2678506fdfe5b508b285383d`;
[Actions run 41413495732](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41413495732).
All seven Linux ASan/UBSan suites and seven Windows suites passed, including
**56 bounded startup checks** with 17 new GetModuleHandleA cases. The fixture
and clean upstream overlay checks passed. CI built and validated desktop repack
**1.0.46.4**. Console deployment completed with code 0 and success true; version
1.0.46.4 was registered, launch returned HTTP 200, and `DesktopMode.exe` was
observed running with that exact package identity. A fresh game file-open trace
through this desktop version remains unobserved; the 85-instruction result
below is from diagnostics loading the same packaged DLL.

Physical Xbox One X, OS **10.0.26100.10448**, diagnostics **0.9.0.0**:

- Deployment completed with code 0 and launch returned HTTP 200.
- The packaged bridge loaded with ABI 1 and no direct fallback. Its SHA-256 is
  `035c1848d560948623bbf89ac01bdb49d38caf12a5650193d44454c599dc95f3`.
- The embedded four-call smoke test passed in 13 instructions. The native
  simultaneous low-address reservation probe also passed and released its
  reservations, as recorded in `x86-runtime.txt`.
- The saved USB game trace advanced from 60 to **85 guest instructions and
  host gates**, returning normally through GetVersionExA and GetModuleHandleA
  before stopping at unimplemented `KERNEL32.dll!HeapCreate`, gate `0x7ff00760`.
  Guest last-error remained zero; no ExitProcess call was reached.

Six host APIs are implemented among 218 import gates. No graphics or gameplay
was demonstrated.

The added API accepts one 32-bit stdcall argument. NULL returns the mapped guest
image base and preserves last-error; guest tests also read its DOS/PE headers at
two fixed preferred bases. Named lookup remains unavailable: any readable ANSI
name with NUL within 512 bytes, including empty and executable names, returns
NULL with `ERROR_MOD_NOT_FOUND` (126). Unreadable or unterminated names stop with
a controlled marshalling fault. No host module handle is returned. Tests cover
stack cleanup, nonvolatile registers, last-error and string boundaries.

## GetVersionExA startup progress, version 0.8

Source commit: `fe36866d43a7f218e54bf1f8d403e88b7cdb3499`;
[Actions run 41410507832](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41410507832).
All seven Linux suites and seven Windows suites passed, including **39 bounded
startup checks** with 21 new guest-executed GetVersionExA cases. Desktop repack
**1.0.46.3** passed WinRT ABI comparison, signature verification and validation
of all **8,858 blockmap files** in CI. Installation on the One X completed:
upload HTTP 202, deployment code 0, success true. Package version **1.0.46.3**
was registered, launch returned HTTP 200, and `DesktopMode.exe` was observed
running with that exact package identity. The user then opened the game through
this desktop update. Its retrieved runtime log confirmed the same
60-instruction GetModuleHandleA stop as the diagnostic app loading the shared
packaged DLL.

Physical Xbox One X, OS **10.0.26100.10448**, diagnostics **0.8.0.0**:

- The packaged bridge loaded with ABI 1 and no direct fallback. Its SHA-256 is
  `25218abca70bfe9b631130a1e31f7cd11f4d7de69b01140f6f22b03902d1709c`.
- The embedded four-call smoke test passed in 13 instructions, and the native
  low-address reservation probe passed.
- The saved USB game startup trace reached and returned normally from
  `KERNEL32.dll!GetVersionExA`, with guest last-error remaining zero. Execution
  advanced from 41 to **60 guest instructions and host gates**, then stopped at
  unimplemented `KERNEL32.dll!GetModuleHandleA`, gate `0x7ff00250`.

The image still has 218 import gates; **five host APIs** are implemented.
There was no successful `ExitProcess(0)`, graphics or gameplay. The version
query uses the explicit virtual Windows 10.0.19045 profile, not the Xbox OS.

## Packaged Win32Bridge and game startup, version 0.7

Source commit: `4d4f2bcebaf465ea46bfee2864a2462004efb7ba`;
[Actions run 41378042706](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41378042706).
All seven Linux checks and seven Windows checks passed. CI produced the test-signed UWP
companion, the patched Win32Bridge, and desktop repack **1.0.46.2**. The repack
passed WinRT ABI comparison, SignTool signature verification and blockmap
validation of all **8,858 files**. The desktop update then installed on the same
One X with deployment code 0. Its launch returned HTTP 200 and a new
`DesktopMode.exe` process was observed. The console registered package version
**1.0.46.2**. Screenshot capture for this shell remains unavailable. The user
subsequently opened the game through the actual desktop file manager; the
retrieved bridge log confirmed the same 41-instruction GetVersionExA stop as
the diagnostic route. This verifies that the desktop file-open path reaches
the patched runtime.

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
