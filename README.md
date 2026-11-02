# Xbox Consoles Desktop Mode

An Xbox Developer Mode desktop project, starting with the existing
[DesktopMode UWP](https://github.com/Pedro1234-code/DesktopMode_uwp) shell and
its experimental Win32Bridge. Initial hardware target: Xbox One, One S and One X.

The upstream desktop is the foundation. We are not recreating its taskbar, Start
menu, file manager or Win32 window system from scratch. It is pinned as a Git
submodule in [`external/DesktopMode_uwp`](external/DesktopMode_uwp).

## Current scope

| Component | Status |
| --- | --- |
| Upstream desktop | Patched repack 1.0.46.3 installed and launched on One X with its process confirmed; the preceding 1.0.46.2 game file-open route was verified |
| Desktop and Win32Bridge source | Pinned upstream source; patched bridge builds and passes ABI comparison; repacks retain the upstream shell and Gecko binaries |
| Native runtime diagnostics | Installed and tested on Xbox One X: both probes passed, returning 42 with error code 0 |
| Portable PE validation | Linux sanitizer tests and Windows host tests passed, including malformed images |
| USB game inspection | Verified on One X: selected USB folder, read `gta_sa.exe`, parsed PE32/imports and retrieved saved report |
| Integer x86 execution | Version 0.8 packaged bridge passed its self-authored PE32 smoke test on One X: 13 instructions and four typed host calls |
| Bounded game startup trace | Version 0.8 returns normally from GetVersionExA and reaches 60 guest instructions and host gates before missing GetModuleHandleA; no gameplay or graphics |
| Patched Win32Bridge | Version 0.8 loads on One X with ABI 1 and no direct fallback; five host APIs implemented; desktop repack 1.0.46.3 passes signature, ABI and 8,858-file blockmap checks |
| Full desktop build | Requires upstream's external Gecko source and compiled runtime; see build guide |
| Steam / Playden integration | Planned; no Steam client or login implementation in this repository yet |
| PC game graphics | Not implemented here; upstream bridge currently renders classic GUI surfaces |

**This is not yet a Steam-on-Xbox release or a general Windows executable runner.**
Upstream reports partial success with selected classic Win32 applications; that
is useful evidence, but does not establish support for Steam or Direct3D games.

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
Its fresh game file-open trace remains unobserved; the 0.8 diagnostic app verifies
the shared DLL's startup behavior.

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
