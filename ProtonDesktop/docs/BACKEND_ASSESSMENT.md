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

The bundled SDL has a WinRT project. That provides a useful renderer/host
reference, not a working Boxedwine64 UWP build. The older
[`worleydl/Boxedwine-uwp` reference](https://github.com/worleydl/Boxedwine-uwp/tree/e08c29a79375cf8695e794131bf0acd23451319d)
describes 32-bit Wine; its emulator MSVC project has no Windows Store /
AppContainer declaration. Its repository name therefore is not evidence of a
finished Xbox port either.

## Rootfs and direct boot contract

The upstream [rootfs recipe](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/tools/rootfs64/build-wine64-zip.sh)
stages Debian bookworm Wine64, glibc, Wine PE/Unix modules, X11 locale data,
fonts and optional custom GL/audio shims into layered ZIPs. The observed Wine
version is 8.0. The Docker tag/package closure is not immutable provenance.
No rootfs ZIP or executable was downloaded for this milestone.

The native host itself is an uncompiled starter. It has not qualified Xbox
suspend/resume or device loss: Direct3D device removal/reset still ends the
session with an error instead of rebuilding the graphics device. The 17 portable
boot-request checks validate argument construction and rollback only; they do
not validate root ownership, artifact provenance or guest compatibility.

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
