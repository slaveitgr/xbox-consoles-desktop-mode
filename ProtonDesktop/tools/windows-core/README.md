# Windows desktop interpreter diagnostic

First build target for the actual pinned `BOXEDWINE_GUEST_X64` core on Windows.
This is **not UWP**, does not link into the app and does not qualify Xbox or Wine.
Passed on Windows x64 in
[CI 41472941389](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41472941389),
source `b0115e6dc53380a06c38b20284cd1d004896b47a`: **907/907 CPU64 checks**,
positive ELF64 guest exit 0 and negative guest exit 9. This remains desktop
Windows evidence; UWP and Wine have not executed.

Requires Windows x64, a Visual Studio 2022 x64 developer environment (MSVC
headers/import libraries), Windows SDK, CMake, Ninja, Git and LLVM **clang-cl**.
The compiler must support native `__int128` lowering; configuration probes the
actual arithmetic/link path. LLVM has a
[Windows x64 i128 code-generation test](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/clang/test/CodeGen/win64-i128.c).
This source-level support is not a substitute for our build and execution tests.

From the ProtonDesktop root in that developer environment:

```powershell
cmake -S tools/windows-core -B build/windows-core -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full"
cmake --build build/windows-core --target boxedwine64-windows --parallel 4
```

Expected executable: `build/windows-core/boxedwine64-windows.exe`.
It supports the upstream `--x64-selftest` and `--x64-run-elf <own-payload.elf>`
diagnostics. The latter's host exit status alone does not prove guest success;
check the exact guest exit record and expected stdout. Root's CI runner handles
the positive/negative payload checks. Do not run a Wine/rootfs workload here.

## Dependency and source boundary

`BOXEDWINE_SOURCE` must be a clean full/sparse Git checkout at
`58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4`. It needs `source`, `include`,
`platform`, `tools/x11`, `lib/sdl2`, `lib/softfloat`, `lib/simde`, `lib/glew` and
`lib/pugixml`. CMake rejects another pin or dirty/untracked files.

- Actual CPU64, KMemory64, ELF64 loader and syscall64 sources are compiled.
- Actual Windows platform and SDL host sources are used; no kernel/syscall stubs.
- SDL2 is built statically from the same pinned repository. No SDL binary,
  rootfs, Wine binary, legacy D3DX SDK, ZIP library or downloader is fetched.
- Native-code JIT, Boxedwine container UI, OpenGL, ZIP and network downloader
  code are disabled/excluded. This does not yet remove desktop Windows APIs.
- SoftFloat is compiled from the pinned source. `BOXEDWINE_64` explicitly means
  host pointer width; `BOXEDWINE_GUEST_X64` separately selects guest architecture.
- `int128_compat.h` supplies only the upstream GCC typedef spelling used by
  clang-cl; it does not emulate or replace the arithmetic.
- `windows_platform_headers.h` supplies genuine multimedia/shell SDK headers
  only to the two Windows platform files which relied on transitive Windows.h
  declarations. Actual `winmm` and `shell32` libraries are linked.
- If clang-cl needs 128-bit division helpers, CMake links its official
  `clang_rt.builtins-x86_64.lib`. It searches the compiler installation and fails
  if unavailable; `-DPROTON_CLANG_BUILTINS=<absolute-library>` selects it explicitly.
- `/Gy /Gw /OPT:REF` allow the linker to discard unused code; `/OPT:ICF` merges
  identical functions. The result is still a real Windows desktop executable.

The upstream checkout is never patched in place. CMake's generated SDL config
and all build products go under `build/windows-core`. No app packaging, signing,
deployment or Xbox operation occurs.

The first Windows CI attempt proved the `__int128` probe links with the installed
compiler-rt, then failed while compiling SDL: its legacy CMake emits GCC's
`-idirafter <path>` for clang-cl, which treats the path as a second source file.
The build-local SDL copy now selects its existing `include_directories` branch
for the MSVC driver. The real source and API behavior are unchanged; the
second attempt passed that stage, then exposed an SDL2.0.12 intrinsic-header
inconsistency: its clang-cl branch undefines SSE/SSE2 but leaves SSE3 enabled,
selecting an audio converter without its intrinsic declarations. The local
header overlay also undefines SSE3 in that same branch, keeping SDL's actual
scalar converter. This changes SDL host conversion selection only; it does not
replace guest instruction emulation. The corrected build/run passed in the
Windows CI result linked above.
The following compiler pass also showed that this old clang-cl branch skipped
`intrin.h`, hiding `__cpuid` and `_xgetbv` declarations from SDL's real CPU probe.
The overlay includes the compiler's genuine intrinsic header before preserving
the scalar SIMD selection; it does not replace CPU feature detection.

The target explicitly selects SDL's real CRT (`LIBC=ON`) and probes standard C
headers which SDL's old Windows configuration skips. It verifies that `stdint.h`
provides a genuine pointer-width `intptr_t` before generating SDL's configuration.
This fixes the incomplete legacy integer fallback instead of editing individual
string/memory functions. Graphics switches use this version's actual unprefixed
option names (`VIDEO_OPENGL`, `VIDEO_OPENGLES`, `VIDEO_VULKAN`).
Its Windows LIBC loop also replaces unconditional function claims with actual
declaration-and-link probes for every listed CRT function. Missing functions
such as `wcslcpy`/`wcslcat` then select SDL's existing implementations. No dummy
implementations stand in for absent system functions.

The sixth CI attempt compiled the real CPU64/kernel/ELF sources and linked SDL;
only two Windows platform files still required explicit multimedia/shell headers.
Those headers are now supplied per source file. The complete executable link
and Windows execution remain unverified until the next CI result.

UWP memory adaptation has its own independent `backend/uwp` target. It cannot
be linked with the unmodified desktop `platform/windows/platform.cpp`, which
already defines those methods. A full UWP core needs that platform split and the
remaining memory, file, threading, audio, lifecycle and presenter work first.
