# Verified baseline

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

The complete upstream desktop has not been built/deployed by us; its external
Gecko dependency remains a separate integration task. Controller reconnect,
keyboard behavior and extended suspend/resume qualification are also outstanding.
