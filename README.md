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
| Upstream desktop | Release 21286 / 1.0.46.0 installed and launched on Xbox One X; user confirmed desktop and taskbar appear |
| Desktop and Win32Bridge source | Pinned upstream source; bridge compiles, independent full desktop build still pending |
| Native runtime diagnostics | Installed and tested on Xbox One X: both probes passed, returning 42 with error code 0 |
| Portable PE validation | Linux sanitizer tests and Windows host tests passed, including malformed images |
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

## Build for Developer Mode

See [`docs/BUILD.md`](docs/BUILD.md) for Windows prerequisites, companion packaging
and the upstream desktop's external dependencies. See
[`docs/XBOX_TESTING.md`](docs/XBOX_TESTING.md) for the actual console checklist.

The first physical Xbox One X run passed on 2026-11-02 (Europe/Athens).
The UWP companion launched, ran both native probes and saved its report.
See [`docs/VALIDATION.md`](docs/VALIDATION.md) for exact scope and build links.
The upstream desktop release was also installed and launched on the same console;
the user confirmed the desktop and taskbar on the TV. This was the upstream
binary, not a desktop built or customized by this repository. See
[`docs/DESKTOP_BASELINE.md`](docs/DESKTOP_BASELINE.md) for its version,
dependencies, controls and remaining checks. Commercial applications have not
been tested by us.

The companion runs only self-authored diagnostic code. Its PE fixture has no
imports, relocations, TLS or CRT and returns `42` through a deliberately tiny
test entry-point contract. A pass proves that narrow contract, not Win32 app
compatibility. It does not execute arbitrary files from your game library.

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
