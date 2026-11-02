# XWindows OS 1.0.49.0

The current desktop is named **XWindows OS** and uses a compact Windows 11
inspired design: centered Start and taskbar, rounded windows, light/dark
surfaces, original X logo and blue wallpaper. MihawkOS attribution remains in
credits and licenses. Package identity, native RPC identifiers and storage
keys are retained for update compatibility.

Existing personalization choices survive the update. The new wallpaper is
available under **Personalization → Background → XWindows Blue**. The physical
capture retained the previously saved dark theme and solid accent background.

## Current build and physical validation

Version **1.0.49.0** was built at source
`546eda5351593fa17a958ea3b43d7a4063668417` in
[Windows run 41448304912](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41448304912),
then installed and tested on the same Xbox One X on **2026-11-02**.
All nine portable Windows test suites passed. Artifact `13144471277` has
SHA-256 `da5ec7806bd77cfc24f89910da82bff7e4aa0e868b2e15d28a6a754381fc8ab9`;
MSIX SHA-256 is
`7f4c1eb032ea22ca40a38e416b091b7bec55b10a68641413dcc2e716b2fe1914`.
Manifest/source metadata, host/bridge hashes, all 91 payload hashes and blockmap
entries were verified before deployment. Installation and launch succeeded.

The native host now wraps EXE surfaces and the separate browser in an internal
UWP window. It supports title-bar dragging, lower-right resizing, minimize and
restore, maximize within the workspace, and a separate fullscreen mode. F11
toggles fullscreen; Escape and a floating exit control leave it. Minimize hides
the surface and disables guest input. One runtime/overlay remains active at a
time; this is not a new multi-process application manager.

The opt-in physical `windowChrome` check called the actual native window
methods and inspected layout after seven transitions:

| State | Window size | Visible/input |
| --- | --- | --- |
| Windowed | 1100×740 | Enabled |
| Maximized | 1920×1032 | Enabled; 48px taskbar retained |
| Fullscreen | 1920×1080 | Enabled |
| Minimized fullscreen | 1920×1080 retained | Both disabled |
| Restore fullscreen | 1920×1080 | Enabled |
| Leave fullscreen | 1920×1032 | Previous maximized state restored |
| Restore windowed | 1100×740 | Original size restored |

A separate `runtimeReturn42` check launched the immutable packaged
`Diagnostics/return42.exe` through the actual `RunFile`/Win32Bridge path and
awaited its task. It returned **42** with no error. This tiny self-authored
fixture has no imports, CRT, relocations or TLS; its result establishes that
launch contract, not arbitrary Windows application compatibility.

Evidence is retained locally under
`artifacts/local-xbox/xwindows-1.0.49.0/`: `xwindows-window-smoke.json`,
`xwindows-runtime-smoke.json`, `physical-validation.json` and package/deployment
metadata. The app again reported native readiness, two accepted messages, zero
rejections/errors, Edge `150.0.4078.112` and a 1920×1080 viewport at DPR 1. The
temporary test configuration was removed, the app restarted, and ordinary
startup revalidated. No Wine DLL was executed. Manual mouse drag/grip behavior
was not part of the automated console state test.

## Earlier design capture, version 1.0.48.0

Validated on an Xbox One X on **2026-11-02**:

- [Windows build and development package](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41441771476) succeeded at source commit `f21da4e921ffc41f5cb678c7d6e7bb77375a21b6`.
- All eight portable Windows host test suites passed. Browser-only UI checks passed 25 existing checks plus four navigation/branding checks, using mocked RPC.
- Artifact `13140492921` has SHA-256 `7e92764fb5352b181f91b1f1d70a4dd99b7e1284edaa8a71035e47794609f379`.
- MSIX SHA-256 is `5b3255e0ec3ee54b988e9b60272e36b04e7a5f760590a4d2695b6914e9cc88e0`. Its manifest, source metadata, host/bridge hashes, 90 payload hashes and blockmap entries were checked before deployment.
- The community development-signed x64 package installed and launched successfully. This is not an official Microsoft signature.
- The live page reported XWindows OS, native JavaScript readiness, two accepted startup messages, zero rejected messages and no startup error.
- Edge WebView2 `150.0.4078.112` reported a 1920×1080 viewport at device-pixel ratio 1; UWP layout scaling was disabled.
- Eight desktop applications were present. No emulated Wine shortcut or Wine auto-launch was enabled.
- An explicit local `shellCapture` test captured the original trusted desktop view as a nonempty 1920×1080 PNG. The image was downloaded and visually inspected.
- The temporary smoke configuration was deleted, the app restarted and normal startup revalidated.

The capture feature accepts no remote script or RPC action: an app-local test
configuration schedules the fixed trusted desktop view after five seconds.
Browser and Wine views are outside that capture path.

## Remaining native Wine work

This release changes the existing UWP/WebView2 desktop. It does not execute a
Wine guest. The actual pinned Wine Explorer source builds as PE64, but a
working Wine folder view and full Wine desktop have not been demonstrated on
Xbox. The next runtime milestone is a real Explorer window browsing a folder;
its dependencies need a consistent Wine/bridge boundary and an isolated guest
thread environment. The future Wine desktop should use adapted Mihawk theme
components matching the XWindows OS appearance. See
[`desktop/wine-explorer/README.md`](../desktop/wine-explorer/README.md).

AE App is a proposed first application port. Its source assessment and planned
installation experience are in [`AE_APP_PORT.md`](AE_APP_PORT.md); no AE Xbox
port or in-desktop application installer is included in this release.
