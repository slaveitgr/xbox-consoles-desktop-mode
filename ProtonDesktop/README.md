# ProtonDesktop

Separate native UWP port starter for Xbox One X. The intended session runs the
real Wine Explorer desktop inside a **native Boxedwine64 interpreter**, presented
through a UWP graphics surface. No HTML desktop and no WebAssembly are used.

**Current milestone: real guest-x64 core running on Linux and Windows, and a
successfully linked UWP diagnostic core using the Store C++ runtime.** The full
Wine backend factory remains unavailable. An opt-in CoreApplication probe is
being built to execute the actual CPU/ELF checks in app context. No ProtonDesktop
Xbox or Wine runtime result exists yet.

The pinned interpreter compiled from 252 translation units. Its CPU64 tests
passed **907/907**, and our own static ELF64 passed arithmetic, memory, branch,
write and exit checks through the actual upstream loader and syscall dispatcher.
A deliberately failing arithmetic control reached guest exit 9. This is Linux
host evidence; Wine and the UWP backend have not run. The Windows build then
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
No ProtonDesktop release, signing or installation has occurred. Two unqualified
rootfs ZIP candidates have now been assembled locally from verified Debian
packages; see `rootfs/README.md` for hashes, limitations and RAM-only storage.
Executed diagnostics so far use only self-authored ELF64 and upstream CPU tests.
No Wine executable has run in this project.

## Next native milestone

Port the Windows-proven guest-x64 interpreter into the UWP API partition, adapt
its remaining host calls and X11 pixel presenter, then repeat ELF64 checks on UWP.
Only after that, import a provenance-checked, licensed Wine64 rootfs and prove a
real Wine application window. Explorer desktop boot and Windows 11-inspired Wine
theming follow that result. One X performance is unmeasured.
