# Wine PE build evidence

**NOT SAFE TO EXECUTE.** The default build produces stock Wine AMD64 PE audit
artifacts. The optional XCDM variant applies the pinned guest-accessor patch.
Neither variant has a qualified Xbox native backend. Building
Explorer and its DLLs does not establish that DLL initialization, Explorer, a
game, or Steam can run in our bridge. Do not deploy or load these outputs.

This Linux driver builds the exact Wine source in `source-lock.json`, then
expands the file dependency set from actual ordinary imports, PE delay imports
and exported forwarders. It never invokes a Windows EXE, initializes a DLL,
runs Wine, installs Wine, or deploys an application. Host configure probes and
Wine's build generators/compiler tools execute as part of normal compilation.
The separate guest-environment audit and the collaborator's header/runtime
changes remain independent work.

The default seeds include Explorer, shell32, user32, gdi32, comctl32, ole32,
oleaut32, combase and their core PE runtime providers. An empty missing-module
list establishes closure for the parsed file dependencies only. Dynamic
`LoadLibrary` calls, COM activation, registry-driven providers, imported symbol
semantics and runtime initialization remain unqualified.

## Inputs and tools

Use a native x86_64 Linux host with Python 3.10+, GCC/G++, GNU make, Perl, M4,
Bison, Flex, pkg-config, dpkg-deb and the ordinary development headers needed
by Wine's configure checks. The driver does not install system packages.
The initial build used Ubuntu 26.10 and the existing official Emscripten SDK's
LLVM tools; no MinGW installation was needed.

Supply the source archive and Ubuntu Autoconf package identified by URL and
SHA-256 in [source-lock.json](source-lock.json). The driver verifies both before
using them. Autoconf is extracted under the build directory with `dpkg-deb -x`;
package installation scripts are not run.

The default compiler directory is `artifacts/toolchains/emsdk/upstream/bin`.
The recorded clang, lld and llvm-ar hashes must match by default. A different
LLVM toolchain can be selected explicitly with `--allow-unpinned-toolchain`;
its actual hashes are recorded and it is identified as unpinned. LLVM's
`llvm-ar` multicall binary supplies the `llvm-dlltool` alias. The same lld binary
supplies `lld-link` and `ld.lld`. The driver creates these aliases locally.

For this checkout, the already-acquired inputs can be used as follows:

```sh
python3 desktop/wine-explorer/pe-build/build_pe.py \
  --source-archive artifacts/reference/wine-1c78596625336a73dbfc035dbbcf0dbdd95b06eb/source.tar.gz \
  --autoconf-deb artifacts/native-wine-pe-build/deps/packages/autoconf_2.73-2_all.deb \
  --build-root artifacts/stock-wine-pe-repro \
  --jobs 6
```

The driver copies the verified source archive into its own work tree. Original
reference sources and tracked runtime/header files are untouched. It restores
the archive's local `tools/gdbinit.py.in` symlink and generates configure,
config.h.in, server/syscall declarations and Vulkan headers using the pinned
generators and bundled XML. The build is configured for x86_64 PE only, with
tests and the unused Unix X11/Wayland/OpenGL/Vulkan backends disabled.

A temporary `/tmp/xwindows-wine-pe-…` symlink keeps spaces in the repository
path out of generated make commands. Outputs remain under the selected build
directory. Reuse only this driver's marked build directories; manually editing
their source/tool trees is unsupported. Reused extracted source/tool bytes are
not rehashed against the archive/package, so reuse is explicitly recorded as
unverified. Use a new empty build root for fresh stock-source provenance.
This is a repeatable build recipe,
not a claim of bit-for-bit reproducibility across hosts.

## XCDM variant

Stock remains the default. To build the collaborator's guest-accessor variant
in a separate source/object tree, add `--variant xcdm` and use
`--build-root artifacts/xcdm-wine-pe-build`. The default output directory also
changes to that location when this variant is selected. A stock tree cannot be
reused as an XCDM tree, or vice versa.

The driver verifies the SHA-256 of
`patches/wine/0001-xcdm-guest-ntcurrentteb-redirect.patch` against
`source-lock.json`, dry-runs it with zero fuzz on the freshly extracted source,
then applies it. Patch application logs and hashes of its three modified source
files are retained. On reuse, both patch identity and those modified files are
rechecked; other reused source/tool bytes remain explicitly unverified.

Only PE compilation receives `-DXCDM_GUEST`. The configured compiler is clang
with explicit `-target x86_64-windows -fuse-ld=lld --no-default-config`.
`compiler-seh-proof.json` records the effective Makefile flags and preprocesses
the pinned `excpt.h` with those flags to require clang >=19, `_MSC_VER`,
`XCDM_GUEST=1`, `USE_COMPILER_EXCEPTIONS` and `-fasync-exceptions`. This checks
compiler/header selection without executing the generated code.

After compiling the full file dependency set, the driver invokes the
collaborator-owned `tools/audit_wine_guest.py binary` with the patched source
and build directory. Its implementation is not changed here. The audit input
excludes the built stock `ntdll.dll` and `win32u.dll` providers and the six
Unix-side modules `bcrypt`, `crypt32`, `dnsapi`, `netapi32`, `secur32` and
`ws2_32`. Their presence in the compilation inventory does not admit them as
guests: the native bridge/replacement implementations remain requirements.
This audit input is a superset, not a guest admission list. It deliberately
includes the built Wine `kernel32.dll` and `kernelbase.dll` for static comparison;
the strict loader profile assigns both modules to the bridge. Strict module
ownership remains authoritative, regardless of this audit's result.
The built Wine ntdll's x64 `NtCurrentTeb` export provides the link library only;
that provider retains native TEB assumptions and must never execute here.

`guest-selection.json` records `audit_candidate_pe` paths and excluded provider
reasons. Its historical filename does not mean those candidates may execute.
`guest-admission-audit.json` is the tool's raw report;
`guest-admission-summary.json` additionally preserves its return code and
expected/reported module coverage. Missing coverage cannot count as a passing
gate. A successful compiler or static audit does not qualify the native seam,
initialization, registry, rendering, exception runtime or Xbox execution.
All execution gates remain blocked in the metadata. The driver's exit status
describes compilation/file closure; inspect the separate static-admission
summary for the audit outcome; neither list nor summary overrides loader
ownership or execution gates.

## Evidence

The build directory contains:

- `NOT-SAFE-TO-EXECUTE.txt`: the execution restriction.
- `build-provenance.json`: source/dependency hashes, actual toolchain hashes,
  configuration and explicit execution/deployment status.
- `commands.jsonl` and `logs/`: exact build commands and results.
- `pe-inventory.json`: binary hashes, imports by name/ordinal, delay imports,
  export forwarders and file dependencies.
- `closure.json`: selected targets and unresolved file dependencies.
- `build/`: PE DLLs/Explorer and host build intermediates.

The inventory can also inspect existing outputs without building or running
them:

```sh
python3 desktop/wine-explorer/pe-build/pe_inventory.py \
  --binary artifacts/stock-wine-pe-repro/build/programs/explorer/x86_64-windows/explorer.exe \
  --output artifacts/explorer-pe-inventory.json
```

The PE inventory intentionally does not implement the collaborator's GS/TLS
audit. No Wine binaries or build dependency packages are committed here.
Wine's corresponding source and LGPL/component notices remain in its verified
source tree; Autoconf/LLVM retain their upstream licenses.

The local Linux build on 2026-11-02 completed the pinned recipe and an
incremental replay: 61 PE files, zero missing parsed file dependencies, and
61 successful metadata parses. A separate metadata cross-check resolved 9,576
ordinary/delay imported symbols and 933 exported forwarder chains within those
files. Those results qualify this compilation/inventory sample only; they do
not qualify runtime behavior. The observed pinned `VERSION` is Wine 11.0.

A separate census of this sample excludes the four stock provider binaries
below and examines the other 57 PE files. It records these deduplicated target
symbols at the proposed bridge boundary; all observed targets are named, with
zero ordinal targets. Route columns can overlap and therefore do not sum to
the union.

| Target provider | Ordinary imports | Delay imports | Exported forwarder targets | Union |
| --- | ---: | ---: | ---: | ---: |
| `ntdll.dll` | 116 | 0 | 34 | 149 |
| `win32u.dll` | 429 | 0 | 48 | 477 |
| `kernel32.dll` | 329 | 0 | 72 | 349 |
| `kernelbase.dll` | 573 | 0 | 24 | 573 |

The ignored output `bridge-seam-requirements.json` contains each name and its
consumer/route evidence. Exported forwarder targets are conservative candidates;
their presence does not prove the export will be called. The census does not
traverse the excluded providers' own forwarders or discover dynamic loads. It
has not compared these requirements to the bridge's exports, and establishes
no bridge symbol resolution or semantic compatibility. The separate
`symbol-resolution-evidence.json` checks the stock Wine exports only.

Run the self-authored metadata/build-helper checks with:

```sh
python3 -m unittest discover -s desktop/wine-explorer/pe-build/tests -v
```

These checks inspect synthetic data and temporary files. They do not execute a
guest program or establish Xbox compatibility.
