# Genuine UWP APP-family core compile/link diagnostic

Optional and initially incomplete. It compiles real CPU64, ELF64, memory and
syscall sources under WindowsStore / `WINAPI_FAMILY_APP`, with the tested
`backend/uwp` memory, clock and directory subset. Unsupported required APIs and
missing Platform methods must fail compilation/linking. There are no fabricated
kernel functions or successful return values for unimplemented services.

From a VS x64 developer environment with Windows SDK, clang-cl, CMake and Ninja:

```powershell
cmake -S tools/uwp-core -B build/uwp-core -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full" -DPROTON_UWP_CRT_DIR="$env:VCToolsInstallDir/lib/x64/store"
cmake --build build/uwp-core --target proton-uwp-core-linkcheck --parallel 4 -- -k 0
```

Use the same pristine pin and source directories as the desktop core. The
official installed x64 compiler-rt library is required, optionally selected with
`-DPROTON_CLANG_BUILTINS=<absolute-path>`. No dependencies are downloaded here.
`compile_commands.json` and build logs identify the actual compile failures;
`qualification-scope.txt` records the limited scope. CMake compiler identification
uses a static library, so it is not counted as proof of arithmetic linkage.
Configuration capability checks are likewise compile/declaration evidence only
in this target; the final executable link must resolve all retained imports.

The pinned SDL common, atomics, events, threads and timer code is compiled.
Optional video, audio, rendering, controllers, host filesystem and dynamic
library loading are disabled through real SDL options, avoiding its C++/CX
presenter. This is not a Wine desktop renderer. The existing build-local SDL
header/CRT compatibility corrections are reused. SDL's unconditional desktop
library list is replaced by WindowsApp so missing APIs remain visible.

The supported `SDL_BUILDING_WINRT=1` switch selects SDL's existing WinRT dynapi
policy; no desktop DLL-loading/message-box fallback is substituted. The desktop
CLI files `source/sdl/main.cpp` and `startupArgs.cpp` are outside this diagnostic
closure, so their process DPI and host-current-directory bootstrap are excluded.
A checked build-local overlay removes only `knativesystem.cpp`'s desktop `main`;
all its genuine host functions and normal worker behavior remain intact. No
global `__TEST` mode or replacement `boxedmain` is used.

The desktop Windows Platform and WinMM sources are excluded. Real backend/uwp
definitions and the explicit limited profile below are used without duplicates.
The Store CRT library directory is required and placed before other library
search paths; final imports must still be checked for APP runtime linkage.

`LinkProbe.cpp` retains the real interpreter/ELF diagnostics. The build does not
run them, start Wine, create a package, sign, install or deploy. Even a successful
link is only APP-family source/import evidence: actual AppContainer and Xbox
execution still require a genuine CoreApplication integration and runtime tests.

## Closure boundary

The executable retains `runX64SelfTest` and `runX64RunElf`. Function/data sections
and `/OPT:REF` discard unused portions of the compiled source, so a passing link
would qualify the diagnostic's retained call graph only. It does not qualify
all guest `KProcess`, VFS, socket or Wine startup paths. The full backend remains
unavailable until those paths are actually linked, their APIs audited, and
AppContainer execution succeeds.

Desktop launcher methods such as `openFileLocation` and `getResourceFilePath`,
and remaining JIT/cache services are still outside this profile. A future full
bootstrap must not silently import desktop Platform or restore excluded code.

## Next-port constraints from the verified link report

The profile overlay changes only disposable build-local source, with checked
single-match edits against the locked revision. Its behavior is intentional:

- Audio is unavailable. No MIDI backend is published, DSP/mixer opens fail,
  and future bootstrap audio-device registrations are omitted. The upstream
  Fake Midi implementation is not compiled into this profile.
- Software bitmap metadata describes one BGRA32 layout. It advertises no host
  GDI, OpenGL, accelerated, window-presentation or double-buffer capabilities.
  This metadata does not implement a renderer.
- Host JIT/code-memory writes throw an unsupported-operation error before
  accessing memory or invoking their callback. Guest code stays interpreted.
- Unavailable host `/proc/stat`, `/proc/cpuinfo` and CPU-frequency sysfs nodes
  are omitted, with their optional reader translation units excluded. Genuine
  CPU count, online CPU nodes and other kernel/VFS paths remain intact. The
  startupArgs bootstrap overlay is generated for inspection but not compiled.
- The real socket wake pair is checked before worker/FD_SET use. Nonblocking
  and thread creation failures close endpoints. Shutdown joins before closing
  endpoints; its stop flag is atomic. Existing broader guest-socket lifecycle
  behavior is not qualified by these changes.
- Affinity runs on the bound worker before guest execution, using the current
  thread, rather than the native HANDLE closed by `std::thread::detach`.

CPU affinity is optional and guarded by `KSystem::cpuAffinityCountForApp`.
Host telemetry must use supported measurements or deliberately omit unsupported
proc/sys nodes; do not copy desktop `NtQuerySystemInformation` calls unchanged.
The shared heap's code-memory branches remain rejected by the interpreter
profile rather than granting executable host pages.

Do not fix `initWindowsAudio` by blindly selecting upstream SDL audio: that
route includes upstream **Fake Midi** success responses. The initial audio-off
profile needs an explicit unsupported capability boundary. `BOXEDWINE_MSVC` is
currently conflated with desktop Windows audio/graphics paths and must be split
from the actual UWP host capabilities.

Portable profile checks (not Windows/AppContainer qualification):

```sh
cmake -S tools/uwp-core/tests -B build/uwp-profile-tests -G Ninja -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full"
cmake --build build/uwp-profile-tests --parallel 4
ctest --test-dir build/uwp-profile-tests --output-on-failure
```

The C++ suite exercises actual profile metadata, audio-off behavior and code
write rejection against pinned upstream declarations. Python fixtures verify
input preservation, unsupported-node omission and rejection of changed patch
anchors. They do not execute guest processes or prove audio, networking or Wine.
