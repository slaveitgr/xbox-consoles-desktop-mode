# XWindows OS desktop and native Wine port

The requested Wine route is native x64 execution through Win32Bridge, outside
BoxedWine. The first integration milestone is the actual Wine Explorer window
with a working folder view. It needs Wine's real shell classes and a consistent
shell namespace; compiling an EXE alone does not provide those services.
Pinned sources, concrete blockers and the build work are in `wine-explorer/`.
A working native Wine Explorer, full Wine desktop, Steam and games have not
been established on Xbox.

The working XWindows OS desktop is a native x64 UWP host with a Windows 11-inspired interface for
Xbox One family Developer Mode. This replaces the compiled Gecko desktop UI;
it does not port the PS5 Wine/Proton backend. Files, Game Library, LAN transfer
and browser use this host. The frontend does not automatically launch Wine.

The separate `boxedwine/` source and ignored payload are retained as research
reference only. That route emulates Linux/x86 and runs 32-bit Wine in WebView2;
it is not the native x64 port. Its private test image was removed from the
console before validating the host fix, and it is not bundled in the MSIX.

The desktop includes a persistent local Game Library with user-selected EXEs,
cover images and per-game notes/input settings. Importing a title does not
establish runtime compatibility. LAN Transfer is opt-in and uses a random
pairing token for actual file APIs, writes only Files/LAN Inbox, preserves
existing filenames and currently limits each file to 16 MiB.

The browser overlay uses official Edge/Chromium WebView2 with tabs, address
bar and navigation controls. It receives no desktop RPC or host objects.
The trusted desktop uses a separate WebView2 mapped only to its packaged
frontend folder. Its native RPC validates both the sending control and exact
local origin/page; its CSP blocks external requests and frames.
The official Microsoft Xbox sample is the integration reference:
[Microsoft's WebView2 Xbox sample](https://github.com/microsoft/Media-App-Samples-for-Xbox/tree/main/WebView2).

The frontend is GPL-3.0-or-later; see `mihawk/COPYING`. Other repository code
keeps its existing licenses. The native host uses C++/CX to consume the pinned
Win32Bridge WinRT metadata directly and native UWP XAML controls; it does not
use desktop window/process APIs. The browser consumes official WinUI 2 and
WebView2 WinRT metadata; SDK archives are pinned by SHA-256 and their runtime
license notices are packaged. The WinUI framework dependency is supplied
beside the main package.

Initialize submodules, install Python Pillow/numpy, then run
`python tools/stage-mihawk.py --assets-only` for local UI inspection.
On Windows with VS2022 C++ UWP/v143 and SDK26100, build the bridge with
`./tools/build-upstream.ps1 -WithX86Backend`, then `./tools/build-mihawk.ps1`.
The XWindows OS workflow builds host checks and a development-signed MSIX.

The new package retains the community DesktopMode identity and application ID
to update an existing install while preserving app data. It is community
signed, not officially signed by Microsoft. Files writes are limited to the
app workspace; picker-approved USB roots are read-only. Notepad acknowledges
successful native writes. EXE launch delegates to the experimental bridge;
Steam and GTA compatibility is not established by this desktop change.

Source assets come from MihawkOS `f81a028a668b36f5bcb51b09e66d526979382f1d`.
Licensed object icons, theme tokens and the Selawik font are staged alongside
our original XWindows OS logo and abstract blue wallpaper. The separately licensed Bubble City image is
not used. `tools/stage-mihawk.py` copies the relevant licenses into the MSIX.

`LocalState/mihawk-desktop-status.json` records native start, page navigation,
JavaScript readiness, WebView2 user agent and any startup error.
Browser mock checks are UI checks, separate from physical Xbox validation.

Physical One X validation of version 1.0.47.2 established native page readiness
with three accepted RPC requests, zero rejected requests, Edge 150.0.4078.112,
and a 1920 by 1080 viewport at device-pixel ratio 1. The previous package's
event-sender wrapper comparison rejected legitimate WebView2 messages; the
fixed handler retains the subscribed core and checks the exact trusted page.
This validates the desktop connection, not Wine execution.

An explicit local smoke configuration then started the real One X LAN listener.
Eight physical checks passed: missing/incorrect tokens, foreign-origin rejection,
file upload, listing, download, downloaded-byte hash, and traversal-name rejection.
The owned test file and smoke configuration were deleted, and the desktop was
restarted to stop the listener. The separate browser initialized Edge 150;
initialization alone does not establish that a public page rendered correctly.

Version 1.0.47.3 was then built and installed on the same One X. It removes the
emulated Wine shortcut and startup preference. The live page reports eight
desktop applications, native readiness and zero rejected messages; the LAN
listener is stopped after the test. No Wine guest was executed on the console.

Opt-in native integration smoke tests are requested only by a local
`mihawk-native-smoke.json` file containing explicit `lan`/`browser` booleans.
They are off by default and never triggered by a website. Remove the file
after testing. Separate desktop, browser and smoke status JSON files under
LocalState record actual results; a successful build is not hardware proof.

The product name is **XWindows OS**. The current frontend uses centered Start
and taskbar controls, compact desktop spacing, rounded window frames and
light/dark surfaces inspired by Windows 11. Original SVG branding lives in
`branding/`; upstream notices remain in the package. The future native Wine
desktop should adapt Mihawk's Wine theme to these same visual choices; the
frontend styling does not implement or prove that Wine desktop.
