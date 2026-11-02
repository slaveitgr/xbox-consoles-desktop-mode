# Mihawk desktop for Xbox

Native x64 UWP host and a focused adaptation of MihawkOS visual design for
Xbox One family Developer Mode. This replaces the compiled Gecko desktop UI;
it does not port the PS5 Wine/Proton backend.

The frontend is GPL-3.0-or-later; see `mihawk/COPYING`. Other repository code
keeps its existing licenses. The native host uses C++/CX to consume the pinned
Win32Bridge WinRT metadata directly and platform UWP XAML WebView; it does not
require WebView2 or desktop window/process APIs.

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
