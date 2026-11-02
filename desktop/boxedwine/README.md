# Real Wine Explorer backend

This integration builds Boxedwine's Linux/x86 emulator and runs the **real 32-bit Wine Explorer** inside its virtual filesystem. It does not implement a replacement Explorer in HTML. The desktop shell remains a separate UWP component. Xbox execution is unverified until the built payload has been tested on the console.

`pins.json` fixes the upstream source commit and Emscripten version. `build-web.py` selects the single-threaded interpreter, disables the native/JIT targets, uses legacy WebAssembly exceptions, disables tail calls, starts Wasm memory at 128 MiB, and caps growth at 1 GiB. This profile does not require SharedArrayBuffer, COOP, or COEP. It avoids the experimental native UWP fork's RWX allocator and desktop-only imports. Performance still needs measurement on Xbox One X.

## Source build

Install and activate the official [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html), version **5.0.7**, matching the pinned upstream CI configuration. Python 3, Git, GNU Make, and an SDK-supported host are required. Use a source directory without spaces because upstream Make uses unquoted source lists.

```sh
python3 desktop/boxedwine/build-web.py \
  --source /tmp/xcdm-boxedwine-source --fetch \
  --output artifacts/boxedwine-web --jobs 4
```

An optional `--rootfs PATH` includes only the exact filesystem reviewed in `pins.json`. No executable, Wine process, or Windows program is launched by the build or static verifier. Demo ZIPs and games are excluded. `build-info.json` records the compiler, build flags, source pin, and all file hashes.

Acquire the reviewed image with `python3 desktop/boxedwine/fetch-rootfs.py artifacts/wine-rootfs.zip`, then pass that path to `--rootfs`. The helper checks the pinned size, SHA-256, all ZIP CRCs and the selected Wine/Explorer component hashes without execution.

## UWP integration contract

Expose the generated payload with WebView2 virtual-host mapping on a **separate origin**, for example `https://wine.local/`. Use a separate WebView2 instance and data directory. Do not add the desktop's native RPC handler, host objects, privileged filesystem access, or message forwarding to this instance. Deny external navigation, new windows, and downloads initially. The guest sees its own Wine C: drive; access to the desktop workspace and Xbox USB storage is not implemented by this backend.

The complete Wine desktop launch URL is:

```text
https://wine.local/boxedwine.html?root=wine-explorer.zip&p=explorer.exe&args=%2Fdesktop%3Dshell%2C1920x1080&resolution=1920x1080&auto=true&sound=false&storage=indexeddb&wine-override=nomono&embed=true
```

This becomes `/bin/wine explorer.exe /desktop=shell,1920x1080` inside the emulated Linux system. Specifying Wine's virtual desktop size as well as the emulator resolution avoids Wine's separate 800×600 default. `embed=true` fills the available area while preserving the guest screen's aspect ratio, hides the upstream debug controls by default, and retains real boot errors. The small ⋯ button or Ctrl+Shift+D opens diagnostics. This wrapper does not draw Wine's desktop or taskbar.

For only the standalone file manager, use:

```text
https://wine.local/boxedwine.html?root=wine-explorer.zip&p=explorer.exe&args=%2Fe%2CC%3A%5C&resolution=1280x720&auto=false&sound=false&storage=indexeddb&wine-override=nomono
```

`auto=false` requires pressing the emulator's Start button. `p=explorer.exe` becomes `/bin/wine explorer.exe /e,C:\` inside the emulated Linux system. Wine's desktop is distinct from the Mihawk Xbox frontend. This is stock Wine Explorer, without Mihawk's custom Wine patches.

Persistent files live in this WebView2 origin's IndexedDB overlay. They are not Windows host paths and do not grant access to other UWP apps. Deleting WebView2 user data removes that overlay. A filesystem/USB adapter requires separate implementation and tests.

## Provenance and licensing

Boxedwine: [danoon2/Boxedwine](https://github.com/danoon2/Boxedwine), GPL-2.0-or-later. The source pin in `pins.json` and these build instructions identify the corresponding source. The payload includes upstream GPL and dependency notices; retain the Emscripten/SDL/zlib runtime notices emitted by the toolchain. Distributors must provide the complete corresponding source and any changes for all included components.

The reference [worleydl/Boxedwine-uwp](https://github.com/worleydl/Boxedwine-uwp) is pinned for audit only. Its native AppContainer project uses SDL/WinRT and Mesa-on-D3D12, but its x64 binary translator requests writable/executable pages simultaneously. The reference fork has no published AppX or successful Actions artifact. Its prebuilt DLLs are not copied by this integration.

The optional root filesystem is the upstream **Wine 11.0 web revision 24** image, 99,512,877 bytes. Its SHA-256 and component identities are pinned in `pins.json` and match upstream `tools/buildWine/webgl_filesystems_v24.json`. Inspection confirms the actual ELF32 Wine runtime, `i386-unix/explorer.exe.so`, `shell32.dll.so`, and initialized C: drive with Explorer and Shell32 PE modules. These files are not Microsoft's Windows binaries.

The image is an unmodified upstream dependency for private console testing. Wine source is [wine-mirror/wine at db11d0f](https://github.com/wine-mirror/wine/tree/db11d0fe6a169c457e23d007e20404643d067aa8). The pinned Boxedwine tree's `tools/buildWine/filesystem_wine11.json`, `wine_builds.json`, `webgl_filesystems_v24.json` and `README.md` describe the build and additions. The image itself retains Wine patch sources, addon licenses, psVoodoo's complete patched source archive, Linux package inventory and component notices. The payload adds Wine's LGPL-2.1 text. The TinyCore base dependency and source reconstruction instructions are identified in the upstream profile. Preserve these notices and make complete corresponding component sources available before public redistribution; the prepared runtime image is kept in ignored local artifacts, not committed to this repository.

Wine root filesystems combine LGPL-2.1-or-later Wine with the component licenses of their Linux userspace. A Wine filesystem is a separate dependency, not part of the Boxedwine engine license. No proprietary Windows Explorer, Steam client, or commercial game is added by this integration.

## Evidence boundaries

- Source inspection can establish architecture and included files.
- A successful source build plus static verification can establish a reproducible interpreter payload.
- Rendering and interacting with Explorer in the Xbox WebView2 instance are required to establish Xbox Explorer support.
- None of these checks establishes Steam installation or game compatibility.
