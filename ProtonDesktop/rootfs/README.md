# Wine64 rootfs inputs and first-boot recipe

This directory now contains a tested offline assembler and the first locally assembled Wine64 base layers for ProtonDesktop. It acquired the 125 exact hash-locked Debian core packages and assembled two real ZIPs in private RAM storage. **Wine has not run, no prepared prefix exists, and the layers are unqualified for boot or redistribution.** The original metadata-only locks remain frozen evidence; current artifact results are separate.

## Frozen inputs obtained

`debian-bookworm.lock.json` records Debian **12.15**, release date **2026-07-11**, architecture **amd64**, component **main**. It pins:

- Wine `8.0~repack-4`: `wine64`, `libwine`, `fonts-wine`.
- glibc `2.36-9+deb12u14`.
- FreeType/fontconfig, X11 runtime and locale data, Liberation fonts, tzdata and the remaining declared dependencies.
- **125 packages** in the explicit seeds' `Depends`/`Pre-Depends` closure, plus one optional ALSA-plugin package reference. Every package has its exact version, filename, byte count and SHA256.
- **107 source packages**, including exact `.dsc`, original source archive and Debian patch archive references and SHA256 values. Source archives themselves have not been downloaded.

The lock is backed by a retained `metadata/InRelease`, all three successful Debian signatures, fingerprint-checked official archive/release keys, and SHA256-addressed `Packages.xz` / `Sources.xz` indexes. The compressed indexes are currently cached under `.cache/` (18 MB, ignored by git). The key bootstrap used Debian's official HTTPS key service and explicitly checked published fingerprints; this is recorded rather than claiming a preinstalled independent trust anchor.

The package dependency walk chooses an already-selected compatible alternative where possible, then the first concrete compatible package from the signed amd64 index. Version comparisons use `dpkg --compare-versions`. It fails on an unsupported/unresolved dependency expression. This resolves declared dependencies; it is **not** an APT install simulation, a check of `Conflicts`, maintainer-script effects, or the final ELF/dlopen payload closure. The optional `libasound2-plugins` reference is not part of the core dependency walk. This lock intentionally uses the one recorded main release, without silently incorporating a moving security/updates repository.

Verify the frozen lock:

```sh
python3 rootfs/verify_metadata_lock.py
```

On a clean checkout, fetch only the exact content-addressed metadata indexes first:

```sh
python3 rootfs/verify_metadata_lock.py --fetch-indexes
```

Both commands download **no** package/source archives or Wine binaries. Verification currently passes against 3 signatures, 126 binary-package records, 107 source-package records and 4 pinned upstream source files. Output is recorded in `metadata/lock-verification.json`.

An intentional future update is separate:

```sh
python3 rootfs/lock_debian_metadata.py --refresh
```

This replaces the lock from newly fetched official signed metadata. It requires Python 3, `gpg`, `gpgv` and `dpkg`, and never installs packages. Keep the current compressed indexes or archive them with the eventual build evidence: Debian mirror by-hash URLs and pool filenames are content-addressed/referenced here, but long-term mirror retention is not guaranteed.

## Local assembler and current artifact evidence

Run the self-authored archive/ELF fixtures:

```sh
python3 -m unittest discover -s rootfs -p test_assemble_rootfs.py -v
```

All **20 tests** pass. They cover exact hash/size rejection, AR/tar truncation, traversal/special-file/link escapes, conflicting and identical package overlaps, hardlink materialization, virtual drive-link encoding, real ELF dependency metadata/missing-library diagnostics, private-library unresolved bindings, and deterministic ZIP bytes on one host. They do not execute Wine or qualify a console runtime.

Assembly is offline by default; missing cached packages cause a clear failure. On a machine with sufficient disk space:

```sh
python3 rootfs/assemble_rootfs.py
```

An explicit dependency acquisition and RAM staging example is:

```sh
python3 rootfs/assemble_rootfs.py --download --work-dir /dev/shm \
  --output /dev/shm/protondesktop-rootfs-local
```

Outputs are restricted to ignored `rootfs/.cache` or a private prefixed RAM directory. Package acquisition and artifact copies preserve at least **512 MiB disk reserve**; extracted payload is bounded to **2 GiB**. The local laptop lacks enough disk space to retain these ZIPs while keeping that reserve, so the actual binary outputs remain in `/dev/shm/protondesktop-rootfs-assembled-vh8718jj/`. This RAM storage is volatile. Only the small status/provenance reports were copied to `rootfs/.cache/assembly-evidence/`.

| Actual artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `glibc-rootfs64.zip` | 2,850,458 | `2507d717e4f860789ea54fa0d73a8db8b152827f74ee24d2665c81757b439822` |
| `wine64.zip` | 276,707,863 | `a2614a5fbda1e4783f30b2e0fb3a66759c2649a1d86250a386191ba43a8cc0ea` |
| `payload-manifest.json` | 3,878,623 | `16c5b0b3a993a7220660072826c98ddb2baf4b97af5a0ba8c8717934b743c3cd` |

The assembly produced **5,941 guest entries** from all 125 core packages, excluding optional ALSA plugins. The report status is `built_unqualified`; CLI exit **2** currently marks the unresolved private-library runtime bindings. It does not hide those edges by inventing a global library search path. Registry initialization, actual Wine startup, prefix shutdown/quiescence and Xbox execution remain untested.

The report also identifies **200 package-notice/common-license gap references**. The package graph does not include Debian's implicit Essential `base-files` payload supplying common license texts, and a complete minimized runtime/license closure has not yet been staged. The assembler does not fetch unlisted packages to hide this gap, and the ZIPs are not qualified for redistribution. The per-package copyright files present in the payload and exact source-package references remain recorded.

## Match the actual pinned upstream

The source pin is Boxedwine64 `58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4`. `upstream-sources.lock.json` hashes four exact source scripts retained under `reference/`; none was executed.

The original `build-wine64-zip.sh` starts from a mutable Docker image named `boxedwine64/wine64-debian:bookworm`, originally populated using unversioned `apt-get install wine64`. It computes a shared-library closure by running `ldd`, copies Wine's whole x64 Unix/PE module trees and Wine/X11 data, then makes `glibc-rootfs64.zip` and `wine64.zip`. That recipe identifies the right layout but does not pin the image digest, package versions, custom guest GL/ALSA prebuilts or ZIP timestamps. Our metadata lock removes package-version ambiguity; it does not pretend the original ZIP bytes are reproducible.

The original `build-prefix64.sh` **requires an existing `prefix64.zip`**. It patches profiles, registry and several unrelated apps/games/32-bit links into it. It does not create the first clean Wine prefix. ProtonDesktop will generate a fresh x64-only prefix and will not reuse that demo prefix or its bundled third-party apps.

## Assembly plan for the next implementation

`assembly-plan.json` is the machine-readable plan. Its three layer hashes remain null until actual artifacts exist.

1. The implemented `assemble_rootfs.py` acquires only the exact core `.deb` files in the lock when `--download` is explicit, verifies each byte count/SHA256 **before** parsing, and retains matching source references. It parses AR and `data.tar` directly without running Wine, `dpkg`, installers or package maintainer scripts. Payload blobs use private content-addressed staging, not host paths from the archive; virtual links become supported `.link` entries. Conflicting package payloads fail instead of silently overwriting each other. Every retained file has a hash/source provenance record.
2. Resolve `DT_NEEDED` recursively from the real `/usr/lib/wine/wine64`, `/usr/lib/wine/wineserver64` and every `x86_64-unix/*.so`, using an ELF parser or `readelf` against extracted files. Do not execute guest binaries to discover dependencies. Add the explicit dlopen-only dependencies listed in `assembly-plan.json`, especially `libgcc_s.so.1` for pthread unwinding and Xcursor/Xrender/Xfixes/Xrandr etc. Close those dependencies too. The initial assembler retains the full selected package payload as an inspectable superset; a minimized distribution has not been implemented. Its real static audit currently parses 107 ELF files from 33 Wine/Unix seeds, with zero malformed ELF files and zero absent library payloads. It separately records 29 runtime-resolution-pending edges for private `ntdll.so`/`win32u.so`: those files are present, but runtime Wine loader/load-order resolution is not proven. An empty `/etc/ld.so.cache` and the qualified default/RPATH search do not by themselves prove these private-library bindings.
3. Preserve real Wine and font/X11 data. Stage `/usr/share/wine`, `/usr/share/X11/locale`, fontconfig configuration and the licensed Liberation fonts. Copy a real UTC TZif from the pinned tzdata package to `/etc/localtime`; an absent zoneinfo target or arbitrary text is insufficient. Supply explicit app-controlled `/etc/passwd` and `/etc/group` entries for `username`, uid/gid **1000**, matching the pinned x64 syscall backend. A files-based NSS configuration and localhost hosts data must accompany them.
4. Produce the two immutable layers below, plus a writable app-owned guest overlay. Stable ZIP construction must sort names, set one recorded UTC `SOURCE_DATE_EPOCH`, explicitly encode directory entries/permissions and use a recorded Python/zlib version. Store a payload SHA256 manifest, package/source mapping and final ZIP hashes. Bit-for-bit reproducibility is unproven until two independent builds are compared.
5. Initialize the clean prefix as described below once the actual UWP core/X11 presenter can run it. A prepared prefix layer is generated output from that real boot, not a directory skeleton presented as initialized Wine.

| Layer | Required contents |
| --- | --- |
| `glibc-rootfs64.zip` | `/lib64/ld-linux-x86-64.so.2`, `/lib/x86_64-linux-gnu/` ELF dependency closure, real NSS/TZ/config data, explicit `/tmp`, `/run/user/1000`, `/home/username`, `/var/tmp` directories |
| `wine64.zip` | `/usr/lib/wine/wine64`, `wineserver64`, `wineapploader`; complete `/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/` and `x86_64-windows/`; Wine/X11/font data and notices |
| `prefix64.zip` | Actual Wine-created registry/drive_c/profile data under `/home/username/.wine`, with supported virtual links; excludes running socket/lock/temp files |

The first profile targets GDI/X11 software pixels. It does not include the upstream custom GL shim, ALSA/OSS plugin payloads, demo games, HxD, Microsoft fonts or Windows assets. Optional GL/audio paths require separate reproducible source builds and matching host backends.

### Link format matters

The pinned `source/io/fszip.cpp` reads virtual links from **`.link` files**. It does not implement a generic Unix-symlink-attribute decoder for ZIP entries. `source/io/fs.cpp` translates `(_colon_)` to `:` and `(_at_)` to `@`.

Therefore prefix drive links must be represented as:

| ZIP entry | File contents | Guest meaning |
| --- | --- | --- |
| `home/username/.wine/dosdevices/c(_colon_).link` | `../drive_c` | `C:` points to the prefix drive |
| `home/username/.wine/dosdevices/z(_colon_).link` | `/` | `Z:` points to the guest root |

Other staged links must use the supported `.link` convention or deliberately dereference the target within the extracted package tree, recording the mapping. Do not depend on privileged Windows host symlink creation or `zip -y` alone. Do not include stale wineserver socket directories, lock files or `regf*.tmp` from a previous session.

## Real first boot and the desktop target

Use the environment from `assembly-plan.json`, matching the current `BootRequest` layout:

```text
HOME=/home/username
USER=username
WINEARCH=win64
WINEPREFIX=/home/username/.wine
WINELOADER=/usr/lib/wine/wine64
WINESERVER=/usr/lib/wine/wineserver64
WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine
DISPLAY=:0
WINEDLLOVERRIDES=mscoree,mshtml=
```

The last setting disables Wine Mono/Gecko prompts/downloads for the initial shell experiment; those components are not present in this plan. Both `BuildPrefixBootRequest` and `BuildDesktopBootRequest` now include this complete environment in their launch descriptions.

The backend will first run `/usr/lib/wine/wine64 wineboot.exe --init` and observe the actual guest process completing within a defined deadline. After successful initialization it can import the original palette with `/usr/lib/wine/wine64 regedit.exe /S Z:\home\username\ProtonDesktop.reg`, again observing that guest process's result. These operations must share the same owned guest kernel, user and `WINEPREFIX`.

The pinned backend changes wineserver lifetime: [`source/kernel/syscall64.cpp`, lines 1676–1689](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/source/kernel/syscall64.cpp#L1676) adds **`-p` to bare `wineserver64`/`wineserver` execs**. That keeps the server alive after its clients exit. The Wine 8 [`wineserver` manual](https://manpages.debian.org/bookworm/wine/wineserver.1.en.html) defines `-w` as waiting for server termination; it is **not a registry-flush command**. Calling `wineserver64 -w` after `wineboot` can therefore wait indefinitely at this pin.

A controlled prefix lifecycle is still a **pending backend policy**, not part of the implemented launch-description contract. It must track the initialization/theming clients, prevent new clients while finalizing, and arrange a qualified clean shutdown of the owned server under an explicit persistence policy. A same-kernel/prefix `wineserver64 -w` may only be an exit-observation step after that controlled shutdown has been initiated, with an enforced host deadline and an observable timeout result. It must not be an unconditional step between launches. This plan does not prescribe `wineserver -k` as a substitute for an implemented shutdown/registry-persistence policy.

Before publishing `prefix64.zip`, prove that all relevant guest clients and wineserver have terminated, workers have quiesced, registry writes have completed and handles are closed. Only then inspect the three actual registry files, `drive_c/windows/system32`, drive links and user-profile directories; exclude transient socket/lock/temp state; build the ZIP and verify its actual payload manifest and final hash. A missing lifecycle policy, timeout, nonzero guest exit, instruction cap, unresolved syscall or incomplete artifact check prevents publication. Do not mask failure with `|| true` or write an update-disable marker to hide initialization failure.

A later desktop launch is `/usr/lib/wine/wine64 explorer.exe /desktop=ProtonDesktop,1920x1080`. It uses its own qualified running-session lifecycle rather than treating prefix-artifact finalization as a background flush.

The registry palette is the existing original `wine-theme/ProtonDesktop.reg`, staged under the guest HOME before import. It gives Wine real colors and a named desktop. **It does not provide Windows 11 window chrome, a taskbar or a complete `.msstyles` theme.** Those require actual Wine theming/shell work after a real Explorer window appears. Upstream itself lists Explorer as experimental; working Notepad is not proof that `explorer /desktop` already works.

Fresh first boot and prepared-prefix import are distinct modes in the implemented launch contract. `BuildPrefixBootRequest` produces `BootKind::PrefixInitialization` with exactly two base layers and `wine64 wineboot.exe --init`; it requires a `WritableOverlayDescriptor` whose `appOwned` and `initiallyEmpty` preconditions are true. Those fields are caller-supplied assertions, not filesystem evidence: the future native backend must verify actual app ownership and emptiness before executing the request. `BuildDesktopBootRequest` retains the three-layer prepared-prefix desktop mode. Neither request builder starts Wine, creates a prefix, imports a rootfs or validates artifact hashes. The real first-boot backend execution, persistence/shutdown policy, prefix generation and subsequent third-layer validation remain unimplemented/unproven.

## Source and notices to carry into the eventual artifacts

Each source record in `debian-bookworm.lock.json` includes its exact Debian source archives and copyright reference. Retain `/usr/share/doc/<package>/copyright` (dereference a package's documented copyright symlink), applicable common license texts, and a file-to-binary-package/source-version manifest for every shipped file. Wine's Debian copyright identifies the main Wine code as LGPL-2.1+ with additional component notices; the whole rootfs is not assigned a single blanket license. Fonts and other runtime libraries retain their own notices. The Boxedwine64 host/source scripts retain upstream GPL notices; see the project's existing third-party licensing material.

References:

- [Pinned Boxedwine64 rootfs recipe](https://github.com/andrewnakas/Boxedwine64/blob/58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4/tools/rootfs64/build-wine64-zip.sh)
- [Debian Bookworm Wine64 package](https://packages.debian.org/bookworm/wine64)
- [Wine Debian copyright](https://metadata.ftp-master.debian.org/changelogs//main/w/wine/wine_8.0~repack-4_copyright)
- [Debian archive signing keys](https://ftp-master.debian.org/keys.html)
