# ProtonDesktop

Separate native UWP port starter for Xbox One X. The intended session runs the
real Wine Explorer desktop inside a **native Boxedwine64 interpreter**, presented
through a UWP graphics surface. No HTML desktop and no WebAssembly are used.

**Current milestone: real Wine execution and a Wine window on Xbox One X.**
The installed native 0.2 application verified both app-local runtime ZIP hashes,
ran `wine64 --version` to guest exit 0, and displayed Wine's initial configuration
dialog at 1920×1080. Device Portal captured the actual console image. The app
then terminated before the main wineboot completed or Explorer was started.
Its recorded app memory budget was 1 GiB, with a later process snapshot near
932 MB of private working set. Memory exhaustion is a hypothesis, not a confirmed
termination reason. After selecting per-package Game mode, the app confirmed a
5 GiB budget, wineboot exited 0, and the Explorer launcher exited 0 with live
guest workers. Its recorded memory usage exceeded the earlier 1 GiB budget.
However the television still showed the app splash; generated XWire frames did
not establish visible desktop presentation. Activation, synchronized first
presentation and retained-frame replay fixes are being qualified in revision
0.2.0.1. A complete Xbox Wine desktop and sustained stability are not established.

A complete, consistently built ZIP-enabled Linux runtime rendered the real Wine
Explorer folder view and taskbar on a 1280×720 canvas; an SDL mouse event opened
Wine's Start menu. This is Linux software-video evidence. Forced controller
shutdown joined all 115 guest workers in a separate real-Wine test, while clean
wineserver shutdown and prefix finalization remain unqualified. The earlier
Xbox diagnostic passed 907 CPU64 checks and both self-authored ELF controls
(guest exits 0 and 9).

The new opt-in native Wine application uses existing SDL/WinRT and the real
pinned kernel/VFS, targeting Wine → wineboot → Explorer in one owned session.
Its complete Windows/UWP build, retained kernel link, native SDL WinRT library
and unsigned package succeeded in
[run 41521265311](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41521265311),
including all six Windows contract tests. That exact build was independently
digest-checked, signed with the existing development certificate, and installed
as a same-identity update preserving LocalState. It includes shared-memory,
file-lock, process-ID, full desktop composition, cross-client input and checked
thread-cancellation overlays. This is a development test, not a public release.
The earlier default host's backend factory remains unavailable.

The pinned interpreter compiled from 252 translation units. Its CPU64 tests
passed **907/907**, and our own static ELF64 passed arithmetic, memory, branch,
write and exit checks through the actual upstream loader and syscall dispatcher.
A deliberately failing arithmetic control reached guest exit 9. This is Linux
host evidence for the interpreter. The Windows build then
passed the same **907/907 CPU64 checks** and both generated ELF64 controls.
See [the Linux proof](docs/HEADLESS_CORE_PROOF.md) and
[the Windows proof](docs/WINDOWS_PORT_PROGRESS.md).

The separate UWP diagnostic now **links successfully** with genuine APP-family
headers and the Store CRT. Real socket, scheduling and string adapters plus
explicit unsupported audio/telemetry/code-memory policies resolved the previous
11 link failures. This proves the retained diagnostic call graph, not all Wine
startup paths or AppContainer activation.

Boxedwine64 has substantive x64 guest support and upstream reports Wine64 GUI
applications on native macOS. Its Windows desktop project does not include the
new guest-x64 source files. The older `Boxedwine-uwp` **does have a genuine UWP
application on its `uwp-compat` branch**, including SDL/WinRT display/input and
storage integration; it runs the older 32-bit guest engine. Our initial review
looked at a different commit and missed that useful implementation. Explorer
in the new guest-x64 fork is still listed as experimental. See
[the source assessment](docs/BACKEND_ASSESSMENT.md).

## Files and checks

- `app/`: C++/WinRT CoreWindow and Direct3D/Direct2D native status/presentation host.
- `core/`: validated Wine desktop and first-prefix boot requests.
- `backend/`: explicit native backend interface and a factory that returns
  unavailable until a genuine UWP implementation is linked.
- `backend/uwp/`: actual FromApp memory adapter and upstream Platform compile
  target. It is separate from the still-unavailable full backend.
- `tools/windows-core/`: actual interpreter Windows build using clang-cl,
  the pinned SDL source and the real upstream kernel/ELF loader.
- `tools/uwp-core/`: separate WindowsStore/APP-family compile/link diagnostic.
  It deliberately fails on required services that are not implemented yet.
- `backend.lock.json`: exact upstream sources and remaining qualification gates.
- `rootfs/`: signed Debian metadata lock, pinned upstream references and a
  Wine64 filesystem/first-boot assembly plan; no binary payloads are bundled.
- `wine-theme/`: original Wine registry palette and theme specification. This
  changes Wine settings only; full Windows 11 window chrome is future work.
- `ProtonDesktop.vcxproj`: Windows x64 UWP build starter. Requires Visual Studio
  2022 C++ UWP tools and Windows SDK 10.0.26100.0. Windows CI qualification is
  tracked in [the port progress](docs/WINDOWS_PORT_PROGRESS.md).

Run the portable checks without downloading or executing upstream code:

```sh
cmake -S . -B build/host -DBUILD_TESTING=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

With Python 3 available, CTest also runs seven source-inventory and diagnostic
result-validation fixtures. They can be run separately with
`python3 -m unittest discover -s tests -p 'test_*.py' -v`.
The actual interpreter build is a separate opt-in diagnostic described in
[the core proof](docs/HEADLESS_CORE_PROOF.md), including clean pinned source fetch.

The UWP project passed its Windows x64 compile check and can be built with
`msbuild ProtonDesktop.vcxproj /p:Configuration=Debug /p:Platform=x64`.
Its current backend remains unavailable even if that host project builds.
For an unsigned compile-only CI check, add `/t:Rebuild /p:AppxPackage=false
/p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false`.
Exact prerequisites and the complete command are in
[Windows host build instructions](docs/WINDOWS_HOST_BUILD.md). Its UWP process
and device lifecycle have not been exercised.

Development source and Windows checks now live on the isolated
[`codex/protondesktop` branch](https://github.com/slaveitgr/xbox-consoles-desktop-mode/tree/codex/protondesktop/ProtonDesktop)
of the existing repository. The `codex/x86-runtime` XWindows OS branch is unchanged.
The diagnostic0.1.0.0 package was locally developer-signed and installed on One X.
No public release or Wine desktop result exists. Two unqualified
rootfs ZIP candidates have now been assembled locally from verified Debian
packages; see `rootfs/README.md` for hashes, limitations and RAM-only storage.
The One X diagnostics used self-authored ELF64 and upstream CPU tests; their
result was retrieved from the installed app, with separate phase logs not yet
downloaded. The later Linux Wine-version test mounted the two real candidate
ZIPs in one managed kernel, ran control ELFs with exits 0/9, then ran the genuine
Wine loader with exit 0 and joined all guest workers before cleanup.
Both runtime ZIPs have also been privately uploaded to this app's `LocalState`
on One X; directory sizes were checked, while on-device SHA-256 verification
and Wine execution await the new application.

## Next native milestone

Build the complete desktop/input/cancellation application with Windows/UWP,
then update the installed app to the native SDL/WinRT Wine session. Observe actual
Wine version, wineboot and Explorer outcomes on One X and verify real desktop
pixels and interaction. Windows 11-inspired theming follows a working desktop.
Wine desktop performance on One X is unmeasured.
