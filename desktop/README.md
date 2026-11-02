# Wine desktop and Mihawk Xbox tools

The requested primary environment is now a real Wine desktop session launched
through the source-built BoxedWine interpreter in an isolated WebView2 view.
The native UWP host supplies Xbox storage/setup, Game Library, LAN and browser
tools. The HTML Mihawk adaptation remains the Xbox tools/setup view.

The first backend uses stock Wine 11 Explorer, desktop, Start and taskbar.
The original Mihawk native Explorer/style patches are pinned for porting in
`wine-explorer/`; they are not yet applied to the runtime image. This does not
establish game or Steam compatibility.

Source build and image provenance: `boxedwine/README.md`. The privately supplied
runtime goes in LocalState/WineRuntime; it is not included in the MSIX or Git.
After validated native connection, the tools auto-open Wine Desktop when its
required files are present. The user can disable this in Personalization.
Wine's virtual filesystem currently remains separate from Xbox USB storage.


Native x64 UWP host and a focused adaptation of MihawkOS visual design for
Xbox One family Developer Mode. This replaces the compiled Gecko desktop UI;
it does not port the PS5 Wine/Proton backend.

The desktop includes a persistent local Game Library with user-selected EXEs,
cover images and per-game notes/input settings. Importing a title does not
establish runtime compatibility. LAN Transfer is opt-in and uses a random
pairing token for actual file APIs, writes only Files/LAN Inbox, preserves
existing filenames and currently limits each file to 16 MiB.

The browser overlay uses official Edge/Chromium WebView2 with tabs, address
bar and navigation controls. It receives no desktop RPC or host objects.
The trusted desktop uses a separate WebView2 mapped only to its packaged
Mihawk folder. Its native RPC validates both the sending control and exact
local origin/page; its CSP blocks external requests and frames.
The official Microsoft Xbox sample is the integration reference:
https://github.com/microsoft/Media-App-Samples-for-Xbox/tree/main/WebView2

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
The Mihawk workflow builds host checks and a development-signed MSIX.

The new package retains the community DesktopMode identity and application ID
to update an existing install while preserving app data. It is community
signed, not officially signed by Microsoft. Files writes are limited to the
app workspace; picker-approved USB roots are read-only. Notepad acknowledges
successful native writes. EXE launch delegates to the experimental bridge;
Steam and GTA compatibility is not established by this desktop change.

Source assets come from MihawkOS `f81a028a668b36f5bcb51b09e66d526979382f1d`.
Only original object icons, start mark, tokens, Selawik font and generated
Aero Meadow artwork are staged. The separately licensed Bubble City image is
not used. `tools/stage-mihawk.py` copies the relevant licenses into the MSIX.

`LocalState/mihawk-desktop-status.json` records native start, page navigation,
JavaScript readiness, platform WebView user agent and any startup error.
Browser mock checks are UI checks, separate from physical Xbox validation.

Opt-in native integration smoke tests are requested only by a local
`mihawk-native-smoke.json` file containing explicit `lan`/`browser` booleans.
They are off by default and never triggered by a website. Remove the file
after testing. Separate desktop, browser and smoke status JSON files under
LocalState record actual results; a successful build is not hardware proof.
