# XWindows OS 1.0.48.0

The current desktop is named **XWindows OS** and uses a compact Windows 11
inspired design: centered Start and taskbar, rounded windows, light/dark
surfaces, original X logo and blue wallpaper. MihawkOS attribution remains in
credits and licenses. Package identity, native RPC identifiers and storage
keys are retained for update compatibility.

Existing personalization choices survive the update. The new wallpaper is
available under **Personalization → Background → XWindows Blue**. The physical
capture retained the previously saved dark theme and solid accent background.

## Build and physical validation

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
