# Source and redistribution boundary

Original project code, geometric tiles and theme source use GPL-2.0-only so a
future combined Boxedwine application can retain the backend's GPL obligations.
The GPL text is in `LICENSE`. Four exact upstream build-script references are
retained under `rootfs/reference/`, with their pinned source/hash manifest and
the upstream GPL text in `rootfs/reference/LICENSE`. They have not been run.
Emulator source remains an external pinned checkout; no Wine/rootfs binaries
are included. Source assessment links do not imply a tested port.

Boxedwine64's top-level license is GPL v2. Its bundled SDL, zlib, font, audio,
OpenGL and other components have their own notices. A later build must retain
all corresponding source, licenses and attribution, including local patches.

The current clang-cl diagnostic links the official LLVM compiler-rt builtins
archive. Its Apache-2.0 license with LLVM exceptions and legacy notices are
retained in `notices/compiler-rt-LICENSE.txt`, taken from the LLVM 19.1.5 source.
The SDL2 zlib-style notice is retained in `notices/SDL2-COPYING.txt`. The pinned
zlib and MiniZip sources are built directly for rootfs ZIP access; their notices
are retained in `notices/zlib-README.txt` and `notices/minizip-LICENSE.txt`. Our SDL
build-local overlays modify the pinned SDL source; their source and attribution
must accompany distributed builds. These notices do not qualify the separate
Debian/Wine rootfs payload for redistribution.

Wine uses LGPL-2.1-or-later with component-specific exceptions/notices. A Debian
rootfs also includes glibc, X11, FreeType/fontconfig, fonts and other separately
licensed packages. The upstream staging script is not a complete distribution
license/source manifest and uses a mutable local Docker image tag. Before any
rootfs is distributed, record exact package versions, artifact hashes, notices,
source locations and applicable source provision requirements. Do not copy the
demo's bundled third-party apps, ROM/WAD data or fonts by default.

System Windows SDK libraries and device-provided fonts are build/runtime
dependencies; no Microsoft artwork, font files or Windows binaries are copied.
