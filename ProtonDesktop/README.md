# ProtonDesktop

Separate native UWP port starter for Xbox One X. The intended session runs the
real Wine Explorer desktop inside a **native Boxedwine64 interpreter**, presented
through a UWP graphics surface. No HTML desktop and no WebAssembly are used.

**Current milestone: real guest-x64 core running on Linux, plus a native UWP
launcher starter.** The Boxedwine64 UWP backend is not linked. The native app consequently
shows an explicit unfinished-backend status instead of a simulated desktop.
There is no built/signed/deployed MSIX or Xbox runtime result yet.

The pinned interpreter compiled from 252 translation units. Its CPU64 tests
passed **907/907**, and our own static ELF64 passed arithmetic, memory, branch,
write and exit checks through the actual upstream loader and syscall dispatcher.
A deliberately failing arithmetic control reached guest exit 9. This is Linux
host evidence; Wine and the UWP backend have not run. See
[the reproducible core proof](docs/HEADLESS_CORE_PROOF.md).

Boxedwine64 has substantive x64 guest support and upstream reports Wine64 GUI
applications on native macOS. Its Windows desktop project does not include the
new guest-x64 source files, and there is no emulator UWP project at the pinned
revision. Explorer is still listed as experimental. See
[the source assessment](docs/BACKEND_ASSESSMENT.md).

## Files and checks

- `app/`: C++/WinRT CoreWindow and Direct3D/Direct2D native status/presentation host.
- `core/`: portable, validated Wine desktop boot request.
- `backend/`: explicit native backend interface and a factory that returns
  unavailable until a genuine UWP implementation is linked.
- `backend.lock.json`: exact upstream sources and remaining qualification gates.
- `wine-theme/`: original Wine registry palette and theme specification. This
  changes Wine settings only; full Windows 11 window chrome is future work.
- `ProtonDesktop.vcxproj`: Windows x64 UWP build starter. Requires Visual Studio
  2022 C++ UWP tools and Windows SDK 10.0.26100.0; not compiled in this Linux task.

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

The UWP project can later be built on Windows with
`msbuild ProtonDesktop.vcxproj /p:Configuration=Debug /p:Platform=x64`.
Its current backend remains unavailable even if that host project builds.
For an unsigned compile-only CI check, add `/t:Rebuild /p:AppxPackage=false
/p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false`.
Exact prerequisites and the complete command are in
[Windows host build instructions](docs/WINDOWS_HOST_BUILD.md); this has not run.

No GitHub remote for this app, publication, package signing, installation or
rootfs download was performed. The diagnostic runs only self-authored ELF64 and
upstream CPU tests. No Wine executable ran. This project does not modify XWindows OS.

## Next native milestone

Port the Linux-proven guest-x64 interpreter to a Windows/UWP library, adapt
its host calls and X11 pixel presenter, then repeat the ELF64 checks on UWP.
Only after that, import a provenance-checked, licensed Wine64 rootfs and prove a
real Wine application window. Explorer desktop boot and Windows 11-inspired Wine
theming follow that result. One X performance is unmeasured.
