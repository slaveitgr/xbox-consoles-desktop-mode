# Real guest-x64 interpreter proof

Observed on Linux x86_64, GCC 15.3.0, 2026-11-02. This is a native host build of
the pinned Boxedwine64 source, not WebAssembly. Guest instructions are still
interpreted. It is not a Windows/UWP build or Wine desktop result.

The build compiled 252 translation units with `BOXEDWINE_GUEST_X64=1`,
`BOXEDWINE_MULTI_THREADED`, `BOXEDWINE_DISABLE_UI` and software memory. Native-code
JIT, desktop container UI, OpenGL, ZIP mounting and network downloading are
disabled. The actual upstream CPU64, KMemory64, ELF64 loader and syscall64 code
are used. No syscall stubs replace that code.

Observed results:

- Upstream CPU64 self-test: **907 passed, 0 failed**.
- Our static ELF64: arithmetic, memory and conditional branches reached its
  expected stdout message and guest `exit(0)`, after 20 interpreted instructions.
- Negative control: changing the expected arithmetic value reached guest
  `exit(9)` after 7 instructions and produced no success message.

The negative control matters: upstream `--x64-run-elf` returns host status zero
after a yielded guest exit, even when the guest status was 9. Our wrapper checks
the guest syscall trace and expected output rather than trusting host exit alone.

Evidence remains locally in `artifacts/headless-core-result.json`,
`artifacts/guest64-smoke.log`, `artifacts/guest64-negative.log` and
`artifacts/guest64-selftest.log`. The JSON records source, payload, executable
and overlay digests. Build products are ignored, not release packages.

## Reproduce locally

Create a pristine full or sparse checkout of
[`58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4`](https://github.com/andrewnakas/Boxedwine64/tree/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4).
Our sparse checkout includes `source`, `include`, `platform`, `tools/x11`,
`lib/sdl2`, `lib/softfloat`, `lib/simde`, `lib/glew`, and `lib/pugixml`.
The remaining `lib/imgui`, `lib/tiny-process`, `lib/zlib` and `project/linux`
directories were available for inspection but their UI/download code is excluded.

For a clean project checkout, these commands fetch source at the locked revision
without relying on our ignored research cache. They create only a local upstream
source checkout, not a GitHub repository for ProtonDesktop:

```sh
git -c core.hooksPath=/dev/null clone --filter=blob:none --no-checkout --depth 1 https://github.com/andrewnakas/Boxedwine64.git external/Boxedwine64-full
git -C external/Boxedwine64-full -c core.hooksPath=/dev/null fetch --filter=blob:none --depth 1 origin 58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4
git -C external/Boxedwine64-full sparse-checkout init --cone
git -C external/Boxedwine64-full sparse-checkout set source include platform tools/x11 lib/sdl2 lib/softfloat lib/simde lib/glew lib/pugixml project/linux project/msvc/BoxedWine/BoxedWine
git -C external/Boxedwine64-full -c core.hooksPath=/dev/null checkout --detach 58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4
python3 tools/inspect_backend.py
```

The inventory lock stores the exact raw Git blob SHA256 and Git object identity
for 15 inspected files. The inspector defaults to the checkout above. It is a
source inventory, not a complete dependency closure or runtime admission list.

Prerequisites are CMake, Ninja, GNU C/C++20, binutils, pthreads and an SDL2 runtime
library. The bundled SDL headers can be used with the installed runtime `.so`;
no global development packages or upstream installers were needed here.
From the ProtonDesktop project root:

```sh
python3 tools/run_headless_smoke.py --source /absolute/path/to/Boxedwine64 --jobs 4
```

The script never downloads dependencies or rootfs files. CMake checks the exact
Git pin and a clean source tree. It applies a one-line patch to a disposable
copy of `loader64.cpp`: casting a packed `e_machine` field to a value before
passing it through a logging template. GCC rejects the original reference bind.
The upstream checkout remains unchanged. The payload is assembled from our
`tools/headless-core/guest64-smoke.S`; it is never launched directly by the host.
Every rerun first invalidates the old result JSON; failures publish a failed
record. Result validation requires one exact guest exit status, bounded yielded
execution and a clean exit line. Python regression fixtures reject status 90
when status 9 is required, ambiguous exits and stale prior success.

## What remains before Xbox

The proof does not exercise Wine, rootfs ZIPs, virtual-process services under a
Wine workload, X11 presentation, gamepads, UWP API partitions or Xbox lifecycle.
The real next port batch is the Windows guest-x64 compile, host memory/file/thread
adaptation and genuine X11 pixel presentation. Then repeat these checks on UWP,
qualify a pinned licensed rootfs, show a real Wine64 window and test Explorer.
All Xbox/Wine admission gates in `backend.lock.json` remain false.
