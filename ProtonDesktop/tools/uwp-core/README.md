# Genuine UWP APP-family core compile/link diagnostic

Optional and initially incomplete. It compiles real CPU64, ELF64, memory and
syscall sources under WindowsStore / `WINAPI_FAMILY_APP`, with the tested
`backend/uwp` memory, clock and directory subset. Unsupported required APIs and
missing Platform methods must fail compilation/linking. There are no fabricated
kernel functions or successful return values for unimplemented services.

From a VS x64 developer environment with Windows SDK, clang-cl, CMake and Ninja:

```powershell
cmake -S tools/uwp-core -B build/uwp-core -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full"
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

The desktop Windows Platform and WinMM sources are excluded. Their unported
services are deliberately undefined; the real backend/uwp definitions are used
without duplicates. A missing frequency/proc/socket/audio/platform helper is a
remaining port task, never a reason to add an empty implementation.

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

At the pinned source, the selected platform definitions still lack these
methods (source inventory, not a final-link error report): `nativeSocketPair`,
`getCpuFreqMHz`, `getCpuCurScalingFreqMHz`, `getCpuMaxScalingFreqMHz`,
`openFileLocation`, `getResourceFilePath`, `setCurrentThreadPriorityHigh`,
`writeCodeToMemory`, `clearInstructionCache`, `procStat`, `getL1CacheSize`,
`setCpuAffinityForThread`. Some relate only to disabled UI/JIT paths. Real
process/socket/procfs users must be resolved with implementations or an explicit
supported-profile policy before the Wine backend is considered available.
