# Native Boxedwine64 port assessment

Inspected 2026-11-02. Pinned source:
[`andrewnakas/Boxedwine64` at 58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4](https://github.com/andrewnakas/Boxedwine64/tree/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4).
The assessment below distinguishes upstream claims from our later
[Linux interpreter proof](HEADLESS_CORE_PROOF.md). No Wine or Xbox execution
result is claimed.

## What x64 support actually means

The fork has a real x86_64 guest interpreter, Linux syscall64 dispatcher,
software guest memory and ELF64 loader. Upstream reports real Wine64 Notepad,
other GUI programs and an OpenGL cube on native macOS arm64, plus a WASM64
browser build. The guest path is controlled by `BOXEDWINE_GUEST_X64`.
It is not simply a 64-bit host running only 32-bit Wine.

However, upstream still lists **Explorer as experimental**. Reported browser
Winefile success does not prove a full Explorer desktop/taskbar, or either
program on Xbox. Native UWP host code also does not make guest CPU execution
native: this x64 path interprets guest instructions and emulates Linux services.
There is no Xbox One X performance measurement here.

Primary evidence:

- [README: actual demonstrations and compatibility limits](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/README.md).
- [CPU64: real decoder/interpreter and 128-bit arithmetic](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/emulation/cpu/cpu64.cpp#L587).
- [Linux syscall64 and virtual kernel surface](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/kernel/syscall64.cpp).

## Concrete Windows/UWP gaps

1. **No current guest-x64 Windows target.** The desktop
   [MSVC project](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/project/msvc/BoxedWine/BoxedWine/BoxedWine.vcxproj)
   has zero references to `BOXEDWINE_GUEST_X64`, `cpu64`, `syscall64`,
   `kmemory64` or `loader64`. Its `x64` configurations refer to host architecture
   and the original guest translator, not this new Wine64 path.
2. **Compiler work.** `cpu64.cpp` uses `__int128`/`__uint128_t` directly.
   The existing MSVC target needs equivalent arithmetic or a qualified clang-cl
   route. A desktop macOS compile is not Windows/UWP compilation evidence.
3. **Host API partition.** The Windows
   [platform implementation](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/platform/windows/platform.cpp#L607)
   uses desktop memory/platform facilities (`VirtualAlloc`, `VirtualProtect`,
   system queries and others). UWP needs allowed equivalents, app-owned storage
   and an explicit API audit. Disable the native-code JIT initially: the new
   x64 interpreter can use non-executable data pages for emulated memory.
4. **Renderer.** The x64 in-process X11 wire server must present genuine Wine
   pixmaps through a UWP-compatible pixel path. The native
   [GL64 bridge](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/opengl/gl64bridge.cpp#L537)
   creates a desktop SDL OpenGL context. It cannot be assumed to work in Xbox
   UWP. First target GDI/X11 pixels via SDL WinRT/D3D11 or the host frame sink;
   exclude GL64 until there is an actual supported graphics adapter.
5. **Entry/lifecycle/storage.** The native GUI picker is
   [macOS-specific](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/sdl/main.cpp#L43).
   Replace it with a UWP entry and direct guest argv. Verify guest fork/exec,
   Unix-domain IPC and guest threads remain emulator operations, while host
   threads, synchronization, suspension/resume, audio and input use UWP services.
   Import rootfs/prefix files into app-local storage rather than assuming raw
   USB paths can be opened by the upstream POSIX-style VFS.

The bundled SDL has a WinRT project. The older
[`worleydl/Boxedwine-uwp` `uwp-compat` branch](https://github.com/worleydl/Boxedwine-uwp/tree/5514667573e8a9f370fbe9c542ab5c682c5f5200)
also has a genuine Windows Store/AppContainer wrapper, `SDL_WinRTRunApp`,
CoreWindow input/video, a D3D11 swapchain, pickers/FutureAccessList and package
paths. Its kernel already includes ZIP/VFS and Wine launch/main-loop code.
Our original review used `e08c29a79375cf8695e794131bf0acd23451319d` and missed
this branch. The blanket conclusion that this repository lacks a UWP app was
incorrect.

These are reusable implementations, not merely a repository name. The old
engine still targets Wine32 / 16- and 32-bit programs: its `x64` means the host
and older translator, not the new `cpu64`/`syscall64`/`loader64`/`kmemory64`
guest engine. Its checked source relies on prebuilt SDL/Mesa/zlib DLLs, retains
RWX `VirtualAlloc` paths and hardcoded `E:\\boxwine\\` storage. No Xbox Explorer
execution result was verified during this review.

Primary old-branch evidence:

- [AppContainer project](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/project/msvc/BoxedWine/uwp/uwp.vcxproj#L43)
  and [SDL/WinRT entry](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/project/msvc/BoxedWine/uwp/main.cpp#L58).
- [WinRT storage/display helpers](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/project/msvc/BoxedWine/libuwp/libuwp.cpp)
  and [D3D11 CoreWindow presentation](https://github.com/worleydl/Boxedwine-uwp/blob/5514667573e8a9f370fbe9c542ab5c682c5f5200/lib/sdl2/src/render/direct3d11/SDL_render_d3d11.c#L737).

Our new host and custom SDL frame sink were integration choices, not changes
inherently required by guest x64. Avoid rewriting the existing virtual kernel,
ZIP/VFS or X11 server. Preserve the working x64 core and qualify its app
execution; reuse the pinned startup/VFS and old WinRT integration where suitable.
Safe worker ownership and explicit launch outcomes remain requirements before
the full Wine session can be enabled.

The inspected SDL WinRT app/video/D3D11 files and UWP project are byte-identical
in the old UWP pin and the new guest-x64 pin. Before extending custom GPU/input
code, compile that existing SDL WinRT library with MSVC/C++/CX and test linking
its public C API to the clang-cl guest-x64 core. SDL must own its CoreApplication
and CoreWindow event pump in that route; it cannot share those with a second
`CoreApplication::Run` or independently controlled swapchain. The custom software
driver remains useful for deterministic pixel tests. This reuse candidate has
not yet been compiled or activated as the new Wine application.

## Rootfs and direct boot contract

The upstream [rootfs recipe](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/tools/rootfs64/build-wine64-zip.sh)
stages Debian bookworm Wine64, glibc, Wine PE/Unix modules, X11 locale data,
fonts and optional custom GL/audio shims into layered ZIPs. The observed Wine
version is 8.0. The Docker tag/package closure is not immutable provenance.
No rootfs ZIP or executable was downloaded for this milestone.

The native UWP host now compiles in Windows CI; see
[the host proof](WINDOWS_HOST_BUILD.md). Device recreation and suspend/resume
handling are implemented but remain untested on a device. The 63 portable
boot-request checks validate argument construction and rollback only; they do
not validate root ownership, artifact provenance or guest compatibility.
The real FromApp memory and host-service adapters passed 49 and 47 checks
respectively in Windows test executables. Those are not AppContainer or Xbox
runtime tests, and the app's full backend remains unavailable.

Our boot request deliberately targets the real guest command:

```
/usr/lib/wine/wine64 explorer.exe /desktop=ProtonDesktop,1920x1080
```

It passes explicit `WINEPREFIX`, `WINELOADER`, `WINESERVER`, `WINEDLLPATH` and
`DISPLAY`, with the glibc, Wine64 and prepared-prefix layers separate from the
writable root. The backend must hash/validate those files and satisfy all gates
in `backend.lock.json` before consuming the request. The current factory returns
unavailable and does not execute that command.

## Next proof sequence

1. Compile pinned guest-x64 core on Windows with a reproducible source list.
2. Build the native UWP adapter with GDI-only X11 pixel presentation and run
   the upstream self-authored ELF64/interpreter tests on UWP.
3. Supply a licensed, pinned rootfs and show a real Wine64 diagnostic/Notepad
   window. Record host/console versions, errors, CPU/RAM and input behavior.
4. Prove actual Explorer `/desktop` or report its concrete failure. Winefile is
   a useful separate file-manager milestone, not proof of Explorer desktop.
5. Apply the original palette, then port compatible Wine theme/chrome patches
   for the Windows 11-inspired appearance. No shell imitation substitutes for
   any failed guest step.
