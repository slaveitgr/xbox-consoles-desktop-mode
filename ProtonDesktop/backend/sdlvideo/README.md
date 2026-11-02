# Native SDL software framebuffer port

This opt-in target builds the actual pinned SDL software renderer and a new
`proton-uwp` video driver. It sends real rendered pixels to an explicitly supplied
native host. It is not linked into the ProtonDesktop app yet and does not start
Wine, run a guest, create a CoreWindow or prove Xbox presentation.

The source revision is
[`58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4`](https://github.com/andrewnakas/Boxedwine64/tree/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4).
The existing source staging instructions in the project must first provide a
pristine `external/Boxedwine64-full` checkout. No download/install script runs
from this target. CMake verifies the revision and rejects modified source.

Build and test the pure Linux pipeline:

```sh
cmake -S backend/sdlvideo -B build/sdlvideo -G Ninja -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full" -DPROTON_SDLVIDEO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sdlvideo --target proton-sdlvideo-tests --parallel 4
ctest --test-dir build/sdlvideo --output-on-failure
```

The tests render a **self-authored test texture, not Wine output**, through real
`SDL_CreateTexture` / `SDL_UpdateTexture` / `SDL_RenderCopy` / software
`SDL_RenderPresent`. They independently check every output pixel, source-copy
lifetime, resize, actual host-provided geometry, rejected/throwing callbacks and
failure without a host. A Linux pass qualifies that pipeline only. Windows APP
compilation, Store CRT imports, AppContainer, device loss and physical Xbox
presentation still require their own tests. This target does not install or
deploy anything.

For the separate Windows desktop diagnostic, use a VS x64 developer environment
with the genuine Windows SDK, clang-cl, CMake and Ninja. Select APP SDK headers,
the WindowsApp umbrella and the installed x64 Store CRT explicitly:

```powershell
cmake -S backend/sdlvideo -B build/sdlvideo-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full" -DPROTON_SDLVIDEO_APP_HEADERS=ON -DPROTON_UWP_CRT_DIR="$env:VCToolsInstallDir/lib/x64/store"
cmake --build build/sdlvideo-app --target proton-sdlvideo-tests --parallel 4
# Inspect imports, then stage only the required genuine installed _APP CRT
# dependencies beside the diagnostic if they are not already available.
ctest --test-dir build/sdlvideo-app --output-on-failure
```

`ConfigurationProof.cpp` rejects a desktop SDK partition, a non-x64 APP build,
missing SDL WinRT restrictions or a pointer-size configuration mismatch. The
normal Windows process may execute the texture tests, but its success proves
**desktop diagnostic execution with APP headers/Store CRT**, not AppContainer or
Xbox. There is no desktop CRT fallback if the Store libraries/runtime are absent.
Final executable imports and the source of staged runtime DLLs must be recorded
separately. No redistributable runtime bundle is supplied by this source target.

With `CMAKE_SYSTEM_NAME=WindowsStore`, the same APP restrictions are automatic;
the test executable is compiled/linked but **not registered for CTest execution**.
Real runtime qualification of that mode requires a packaged, activated host.
The generated `qualification-scope.txt` records the selected mode. These Windows
paths are prepared build targets; a Linux pass does not claim they were run.

`ProtonSdlVideo.h` is the versioned C host ABI. Register `query_display` and
`present` before SDL video initialization and retain their userdata until video
quit and reset. Configure/reset during an active session is rejected. The SDL
owner thread invokes both callbacks; the geometry callback must use a real,
thread-safe snapshot supplied by the native host. Missing/invalid geometry fails
initialization. This driver never supplies a made-up desktop size or focus.

The frame is ARGB8888 on little-endian x64, whose memory bytes are BGRA. The
`present` callback must copy all rows to host-owned storage before returning 1.
Frames are bounded to 8192 pixels per dimension, with validated stride. No
callback pointer or SDL pixel pointer may be retained after session teardown.
The native UWP integration must use a bounded latest-frame mailbox, then present
on the CoreWindow/GPU thread; it must not block SDL on synchronous UI dispatch.

`ProtonSdlVideoCopyError` exposes the persistent first failure. Geometry/frame
callback exceptions are contained at the C boundary, and failed sessions stop
forwarding frames. Successful inactive Configure starts a new session and clears
the latch. SDL's software `RenderPresent` returns void and ignores framebuffer
update errors, so an eventual backend **must inspect this latch** and terminate
or report a failed session. A successful callback is acknowledgment of a copied
frame, not proof that a GPU displayed it.

`ProtonSdlVideoCreateSoftwareRenderer` selects real software rendering, disables
VSYNC and disables texture-framebuffer acceleration, preventing recursive
renderer creation. Host swap-chain presentation can provide pacing later. One
logical SDL framebuffer window maps to the existing native host surface.
Exclusive display-mode switching, extra host windows, GL and Vulkan are rejected.
Surface resize reallocates real SDL pixel storage; the geometry pump refreshes
the actual host display snapshot. The driver publishes no keyboard/mouse focus
and does not inject input. Controllers, text/IME, clipboard, audio and native
cursor integration are not implemented here.

`prepare_overlay.cmake` applies checked single-match edits to a disposable SDL
copy: custom-driver registration/configuration and exclusion of the desktop
Windows video source selection. It reuses the existing clang-cl/header/CRT
corrections and fixes the legacy unquoted GCC include path for workspace paths
containing spaces. Two observed opaque-alpha signed shifts are changed to
defined unsigned packing in SDL's real pixel/render functions. The explicitly
static build disables SDL's replaceable-DLL dynapi table through a labelled
source condition; updates require rebuilding the host. This avoids retaining
export entries for deliberately disabled subsystems. The SDL pointer-size value
uses the actual compiler probe rather than the old incorrect Windows x64 LP64
guess. SDL VIDEO, RENDER, EVENTS, THREADS and TIMERS remain real;
desktop X11/Wayland/Windows video, GL/Vulkan, audio and joystick backends are off.

Future XWire integration can retain
[`xwirepresentSDL.cpp`](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/x11wire/xwirepresentSDL.cpp)
and `KNativeScreenSDL`, with software renderer flags. The upstream
"Starting Wine" loading bitmap must be separated from actual guest-frame proof.
No such integration is performed by this target.

The C driver's allocation/lifetime code derives from SDL's dummy framebuffer;
its original SDL copyright and zlib-style notice are preserved in the source,
and the alterations are labelled. It implements real host output, rather than
exposing the dummy driver's discarded updates or fabricated display mode. The
new bridge, headers, build code and tests use GPL-2.0-only like this project.
