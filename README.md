# XWindows OS

A desktop application for Xbox One / One S / One X Developer Mode, with a
Windows 11-inspired interface. The working UWP host provides Files, Notepad,
Game Library, authenticated LAN transfer and a separate Edge WebView2 browser.

The native Wine goal is the real Wine Explorer, followed by Wine desktop,
Start and taskbar with the same XWindows OS visual design. This requires a
Wine-to-UWP runtime port; it is not yet working Wine on Xbox. MihawkOS supplies
licensed reference code and theme components, with its credits preserved.

## Current scope

| Component | Verified status |
| --- | --- |
| Native UWP desktop | Version 1.0.47.3 installed on One X; native RPC ready, zero rejected startup messages, 1920×1080 viewport at DPR 1 |
| Product and design | XWindows OS branding and a compact Windows 11-inspired desktop replace the previous product presentation |
| Edge WebView2 | Trusted desktop runs on Edge 150.0.4078.112; separate public browser initialized on One X |
| LAN transfer | Physical upload/download and content hash passed, with missing-token, wrong-token, foreign-origin and traversal-name rejection; listener stopped after testing |
| Wine Explorer | Actual pinned Wine source builds as a PE64 guest; 64 imports across 8 DLLs, including 13 ordinals; no Wine guest execution on Xbox yet |
| Native Wine environment | Source/PE audit precedes guest PEB/TEB and consistent module-provider implementation; not implemented or hardware-qualified |
| Experimental Win32Bridge | Retained from pinned DesktopMode UWP, with a bounded PE32 startup backend; not a general Windows compatibility guarantee |
| Steam and commercial games | Installer/game startup traces only; no established Steam UI, game graphics or gameplay |

The current desktop build and native Wine source work are documented in
[`desktop/README.md`](desktop/README.md) and
[`desktop/wine-explorer/README.md`](desktop/wine-explorer/README.md).
The earlier runtime measurements below remain historical evidence, separate
from the desktop appearance and the proposed native Wine port.

## Get the source

```sh
git clone --recurse-submodules https://github.com/slaveitgr/xbox-consoles-desktop-mode.git
cd xbox-consoles-desktop-mode
```

For an existing clone:

```sh
git submodule update --init --recursive
```

## Run portable tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

These tests check the constrained parser and platform behavior. Linux reports
native execution as **Unsupported**, never as an Xbox pass. Windows host results
must also be recorded separately from physical Xbox results.

The separate game preflight parser inspects both PE32 and PE32+ headers and
imports without executing them. Its tests include malformed input. Metadata
recognition does not establish game compatibility.

## Build for Developer Mode

See [`docs/BUILD.md`](docs/BUILD.md) for Windows prerequisites, companion packaging
and the upstream desktop's external dependencies. See
[`docs/XBOX_TESTING.md`](docs/XBOX_TESTING.md) for the actual console checklist.

The first physical Xbox One X run passed on 2026-11-02 (Europe/Athens).
The UWP companion launched, ran both native probes and saved its report.
See [`docs/VALIDATION.md`](docs/VALIDATION.md) for exact scope and build links.
The upstream desktop release was also installed and launched on the same console;
the user confirmed the desktop and taskbar on the TV. Our patched bridge repack
1.0.46.2 was subsequently installed and launched, retaining that shell and Gecko
payload. See
[`docs/DESKTOP_BASELINE.md`](docs/DESKTOP_BASELINE.md) for its version,
dependencies and controls, and [`docs/VALIDATION.md`](docs/VALIDATION.md) for
the current bridge and startup-trace results. Commercial game gameplay and
graphics have not been demonstrated.

The native execution probes run only self-authored diagnostic code. The original native PE fixture has no
imports, relocations, TLS or CRT and returns `42` through a deliberately tiny
test entry-point contract. A pass proves that narrow contract, not Win32 app
compatibility. These native probes do not execute files from your game library.

Version 0.3 adds a **Game preflight** page. Choose the folder containing
`gta_sa.exe`, including a USB folder exposed by the device picker. The app reads
the executable in place and saves `game-preflight.txt` to its own LocalState;
copying the game through the desktop is unnecessary. The system picker and USB
grant were verified on the actual One X, including a saved metadata report. See
[`docs/GAME_PREFLIGHT.md`](docs/GAME_PREFLIGHT.md) for controls and report scope.

Version 0.4 adds a separate, automatically running **x86 / Win32 smoke test**.
It interprets an embedded PE32 program and handles four explicit Win32 calls;
it does not execute the USB game. The integer interpreter has no FPU/MMX/SSE,
Direct3D or general Windows API layer. See [`docs/X86_RUNTIME.md`](docs/X86_RUNTIME.md).

Version 0.5 adds **Trace game startup** (**G**). It reads the selected USB game's
PE32 image through the saved grant, maps bounded virtual memory and interprets
up to 50,000 guest instructions. Unknown imports stop at their synthetic gates;
no arbitrary native function is called. `game-startup.txt` records the actual
stop reason. The trace runs automatically when a saved folder is available.
This limited integer-only startup is not a game compatibility or gameplay pass.

Version 0.6 loads that backend from the packaged **Win32Bridge.Runtime.dll**.
The same overlay adds PE32 startup to the existing desktop's executable loader.
CI also produces a separate developer-signed desktop update after
checking its WinRT ABI against the original package. It retains the original
shell and Gecko binaries; a complete independent shell build is still pending.

Version **0.7** was tested on One X with the packaged bridge, without a direct
runtime fallback. The saved USB game executed 41 guest instructions and
host gates before stopping at `KERNEL32.dll!GetVersionExA`. The previous
guest W+X-section and read-only-IAT loader stops are fixed. All 218
imports are bound to gates, while only four host APIs are implemented; this
is not a successful game launch. The corresponding **1.0.46.2** desktop repack
passed CI signature, ABI and blockmap validation, then installed successfully
and launched on the One X. The user then opened the game through the actual
desktop file manager; its retrieved bridge log confirmed the same 41-instruction
GetVersionExA stop, verifying the desktop route as well as the companion.

Version **0.8** implements `GetVersionExA` with a virtual Windows 10.0.19045
profile. On the One X, the saved USB trace returns normally from that call with last-error
zero and advances to **60 instructions and host gates**, stopping at missing
`KERNEL32.dll!GetModuleHandleA`. Five APIs are implemented among 218 import
gates; there is no successful game exit, graphics or gameplay. All seven Linux
and seven Windows suites pass, including 39 startup checks with 21 new version
query cases. Desktop update **1.0.46.3** passes CI signature, ABI and blockmap
validation and installed on the One X with deployment code 0. Launch returned
HTTP 200 and the desktop process was confirmed under package version 1.0.46.3.
The user then opened the game through that desktop; its retrieved runtime log
confirmed the same 60-instruction GetModuleHandleA stop as the diagnostic app.

Version **0.9** adds `GetModuleHandleA(NULL)`, returning the mapped guest image
base while preserving last-error. Named module lookup remains unavailable:
validated ANSI names return NULL and error 126; unreadable or unterminated
strings cause a controlled fault. No host module handles are exposed. All seven
Linux ASan/UBSan suites and seven Windows suites pass, including **56 startup
checks** with 17 new module handle cases. Diagnostics **0.9.0.0** now loads the
packaged bridge on One X and advances the saved USB trace from 60 to **85
instructions and host gates**, returning normally through GetModuleHandleA
before missing `KERNEL32.dll!HeapCreate`. Guest last-error remains zero; no
ExitProcess call, graphics or gameplay is reached. Six APIs are implemented
among 218 import gates. The native simultaneous low-address reservation probe
also passes on the console. Desktop repack **1.0.46.4** passes CI and installed
with deployment code 0; launch returned HTTP 200 and its matching desktop
process was confirmed. A fresh game file-open trace through this desktop
version remains unobserved; diagnostics confirms the shared DLL's
85-instruction HeapCreate stop.

Version **0.10** accepts bounded SECURITY directory 4 certificate metadata in
StartupTrace, fixing the loader rejection for the official `SteamSetup.exe`.
Certificates remain outside guest memory and their signature/trust is not
verified; strict fixture, TLS and relocation restrictions remain. The host
interpreter and Xbox One X companion both execute 13 Steam installer instructions
and host gates before missing `KERNEL32.dll!SetErrorMode`, without reaching
ExitProcess or installer UI. The companion reads the official installer in
place from the saved USB folder using `LocalState/startup-target.txt`. A malformed
override was verified to stop without invoking the runner or falling back to
GTA; the default GTA trace remains at 85 instructions / HeapCreate. All seven
Linux ASan/UBSan and seven Windows suites pass, with 323 PE32 checks, 56 startup
checks and 18 separate target-parser checks. Both CI jobs succeeded. The console
also passed the original four-call smoke and native simultaneous low-address
probe. Desktop repack **1.0.46.5** passes signature, ABI and 8,858-file blockmap
validation, installed with deployment code 0, and launched with HTTP 200;
its matching desktop process was confirmed. A fresh SteamSetup file-open trace
through that desktop remains unobserved; the physical 13-instruction
SetErrorMode stop was verified through diagnostics sharing the exact DLL.
Steam installation and client functionality remain unavailable.

## Development direction

1. Make the full upstream desktop build reproducible, including its external Gecko inputs.
2. Qualify the installed desktop's input, built-in apps and experimental bridge on Xbox.
3. Add repeatable import-coverage and classic-app regression checks.
4. Evaluate Playden's Steam library/auth/download components separately.
5. Add a graphics backend only after defining a small, testable game target.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/UPSTREAM_AUDIT.md`](docs/UPSTREAM_AUDIT.md) for the implementation boundary.

## Attribution

Original additions in this repository are MIT licensed. The desktop submodule
retains its own MIT license and all upstream notices. Other referenced projects
(including GPL-licensed Playden and Wine-related components) are not bundled or
relicensed by this repository. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
