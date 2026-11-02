# Wine guest environment (TEB/PEB, SEH, registry)

Scope: what stock Wine PE modules need from the process they run in, beyond
their imports, so that Wine Explorer can run on Win32Bridge. The loader
(module ownership, import resolution) is a separate work item with its own
owner; this document lists loader findings without changing loader code.

Pinned Wine: `ValveSoftware/wine` `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`
(the MihawkOS base). Explorer dependency closure: 56 modules from
`programs/explorer` `IMPORTS`/`DELAYIMPORTS`, recursively.

## Findings

### Direct TEB/PEB access

`tools/audit_wine_guest.py source` over the closure (tests and Unix sides
excluded):

| Module | Fields read or written directly |
| --- | --- |
| user32 | `Peb->KernelCallbackTable` (written in DllMain), `Peb->SessionId`, `Peb->ProcessParameters->ImagePathName`; `Win32ClientInfo` via inline `NtUserGetThreadInfo()` |
| gdi32 | `Peb->GdiSharedHandleTable` (reads handle entries directly), `GdiBatchCount` |
| ole32 / combase | `ReservedForOle` |
| rpcrt4 | `ReservedForNtRpc` |
| ws2_32 | `WinSockData` |
| kernelbase / kernel32 | `Tib` stack bounds and `FiberData`, `TlsSlots`, `TlsExpansionSlots`, `FlsSlots`, `Peb->Tls*Bitmap*`, `StaticUnicodeString`, `CurrentLocale`, `DeallocationStack`, `ActivationContextStack*`, `Peb->ProcessParameters` (std handles, environment, console), `Peb->Ansi/OemCodePageData`, `Peb->LdrData`, `ClientId` |
| msvcrt | `Tib.ExceptionList` (i386 file only), `Tib.StackLimit`, `Peb->ImageBaseAddress` |
| every module | inline `GetLastError`/`SetLastError`/`GetCurrentThreadId`/`GetProcessHeap` from `winbase.h` under `__WINESRC__` read TEB slots 13, 9, 12→6 directly |

The source scanner reported **0 literal sites** for raw `gs:`, `__readgsqword`
and `__thread`/`__declspec(thread)` in the selected PE sources. The observed
TEB accesses use `NtCurrentTeb()`. This is source-pattern evidence, not proof
that every built instruction or compiler-generated access uses the redirect.

Without the redirect, `NtCurrentTeb()` is inline `mov %gs:0x30` and guest
code would write `Win32ClientInfo`, `KernelCallbackTable`, `LastErrorValue`
and the rest into the **host** UWP thread's TEB/PEB.

### Exceptions

The source scan found **212 `__TRY` occurrences** (crypt32 97, kernelbase 23,
sechost 22, wintrust 16, msvcrt 15, kernel32 13, rpcrt4 9, user32 6, ...).
This includes architecture-specific files and conditional branches; it does
not count compiled x64 exception scopes.
Unless the compiler provides SEH, Wine implements `__TRY` with
`__wine_setjmpex` and a frame pushed on `Tib.ExceptionList`. On x64 only
Wine's own ntdll dispatcher walks that list (`signal_x86_64.c`, "TEB
handler"); the Xbox OS dispatcher does not. Every guarded pointer probe
would turn into an unhandled fault.

`include/excpt.h` enables `USE_COMPILER_EXCEPTIONS` for x86_64 only with
`_MSC_VER` and clang >= 19. Wine's configure adds `-fasync-exceptions` when
supported. Then `__TRY` becomes native `__try`, whose unwind handler is
`ntdll!__C_specific_handler`. The bridge already forwards that import to the
host implementation (`MsvcrtShims.cpp`).

### Modules with Unix sides

`bcrypt`, `crypt32`, `dnsapi`, `netapi32`, `secur32` and `ws2_32` declare a
`UNIXLIB` and call `__wine_unix_call`. As stock modules they cannot work. They
must be bridge-owned, or their Unix side must be reimplemented. Explorer
reaches them mostly through delay imports.

## What is implemented

### NtCurrentTeb redirect: `patches/wine/0001-xcdm-guest-ntcurrentteb-redirect.patch`

- `include/winnt.h`: with `XCDM_GUEST` defined, outside `_NTSYSTEM_` (ntdll)
  and `WINE_UNIX_LIB`, `NtCurrentTeb` becomes an `ntdll` import instead of
  `gs:[0x30]`. Non-x86_64 builds are a compile error.
- `dlls/ntdll/ntdll.spec` + `thread.c`: x86_64 `NtCurrentTeb` export, so
  import libraries link. On a normal prefix it returns the native TEB.
- Verified by compiling Wine-source code with the patched headers: stock
  headers produce 6 `gs:` reads, `XCDM_GUEST` produces **0** and six
  `__imp_NtCurrentTeb` calls; `_NTSYSTEM_` keeps the native form.

### Guest TEB/PEB: `runtime/GuestEnvironment.{h,cpp}`

- `Teb64`, `Peb64`, `ProcessParameters64`, `PebLdrData64` and the GDI handle
  entry use the exact Wine x64 layout: 160+ `static_assert` offsets, sizes
  0x1838/0x7c8/0x410. `tools/verify_guest_layout.py` also compares them
  against the pinned `winternl.h` offset comments: **187 fields, 0
  mismatches**.
- The separate `desktop/wine-explorer/guest-abi/qualify_abi.py` compiles the
  actual hash-pinned Wine declarations against the model. Linux GCC 15
  passes 596 static assertions: size/alignment/standard layout for 15
  records, offsets and widths for 187 addressable TEB/PEB fields plus 82
  supporting fields, and boundary checks for three PEB bitfield words.
  A deliberate one-byte offset mismatch is rejected as a negative control.
  Bitfield bit order, MSVC/UWP and Xbox behavior are outside this result.
- `GuestProcess::Create` builds a PEB with process parameters (normalized
  pointers, environment block, current directory, command line), loader data
  with empty self-linked lists, TLS bitmaps (64 and 1024 slots), the process
  heap list, OS version 10.0.19045, processor count and affinity, code pages,
  session id and a zeroed 1.5 MiB `GDI_SHARED_MEMORY` table.
- `AttachCurrentThread` creates one TEB per (host thread, guest process).
  It sets `Self`, `Peb`, `ClientId` (virtual PID, host TID), locale,
  `StaticUnicodeString`, the activation-context stack pointer and the TLS
  links. A host thread is bound to at most one guest process. Release happens
  on `DetachCurrentThread` or automatically at thread exit (FLS callback on
  Windows, pthread key on POSIX; C++ `thread_local` destructors proved
  unreliable on mingw).
- Host fibers are not guest execution contexts. The cached thread binding
  must match the current OS exit-storage slot before an accessor, attach or
  detach can use it. A host fiber switch therefore returns no guest TEB and
  rejects attachment while another fiber owns the binding. Exit callbacks
  invalidate the matching cache before releasing its TEB. Destroying a
  process while its binding belongs to another live fiber aborts; this is a
  guard against unsupported use, not guest fiber support. Attached contexts
  and their cleanup must remain on the owning host thread.
- `xcdm_guest_current_teb()` is the body of the seam's `ntdll!NtCurrentTeb`.
  It syncs `StackBase`/`StackLimit`/`DeallocationStack` from the host thread
  on every call (guard-page growth stays visible) and returns `nullptr` for
  unattached threads.
- Left zero on purpose: `Win32ClientInfo`/`Win32ThreadInfo` (owned by
  user32/win32u), `GdiBatchCount` (non-zero means "WoW64 TEB64 pointer" to
  Wine), `ThreadLocalStoragePointer` (implicit TLS is rejected),
  `KernelCallbackTable` (user32 writes it).

### Admission audit: `tools/audit_wine_guest.py binary`

Run it on the XCDM build output before any module executes:

```sh
python3 tools/audit_wine_guest.py binary <xcdm-pe-output> \
    --wine-source <wine> --wine-build-dir <wine-build> --json audit.json
```

It fails on decoded `gs:` instructions, inconclusive executable-byte coverage,
a TLS directory, malformed unwind metadata, source `__TRY` with no confirmed
compiler-SEH handler, and a build
configuration that is not clang >= 19 in MSVC mode with `-fasync-exceptions`
and `-DXCDM_GUEST`. It reports, per module, the `ntdll!NtCurrentTeb` import,
the imported/local exported unwind handlers, the `WINE_REGISTRY` resources,
and the aggregated `ntdll`/`win32u` import list. This import union is a
conservative requirement inventory, not the exact implemented/runtime seam.
The scan decodes sections sequentially so operand bytes are not independent
instruction starts. It does not recover control flow or alternate boundaries;
a clean scan alone is not execution qualification. See the actual 53-file
outcome and remaining two inconclusive CRT scans in
[`WINE_XCDM_BUILD.md`](WINE_XCDM_BUILD.md).

### Registry seed: `tools/wine_registry_seed.py`

Offline replacement for wineboot's two registry steps:

- **INF:** `wine.inf` `AddReg`/`DelReg` for `DefaultInstall.ntamd64`, with
  `Needs=`, `[Strings]`, DIRIDs and every `FLG_ADDREG` form that wine.inf
  uses, including `REG_LINK` (0x60000).
- **Resources:** the `WINE_REGISTRY` scripts of the `[RegisterDllsSection]`
  modules. This is a port of `dlls/atl/registrar.c` (register direction).
  HKCR is stored under `HKLM\Software\Classes`.

Output is JSON (`xcdm-registry-seed/1`) for GuestRegistry seeding.
`--require-clsid` gates the Explorer classes: ExplorerBrowser
`{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}`, ShellFSFolder, ShellDesktop and
MyComputer. Run against the pinned `wine.inf.in`, it produces 269 keys. Use
the generated `build/loader/wine.inf` for real seeds.

Eight closure modules do more in `DllRegisterServer` than replay resources.
That extra work is **not** replayed offline: `shell32` (`SHELL_RegisterShellFolders`,
which Explorer needs for shell folder paths), `ole32`, `oleaut32`, `oleacc`,
`urlmon`, `windowscodecs`, `cryptnet` and `wintrust`. The tool warns about
them when given `--wine-source`. Plan: once the guest environment runs,
call those eight `DllRegisterServer` exports once inside the guest at first
boot, on top of the offline seed.

## Loader findings (for the loader owner; no loader code changed here)

1. **Unwind registration: covered.** `RuntimeImage::FinalizeProtections`
   calls `RegisterUnwindMetadata` → `RtlAddFunctionTable` for every mapped
   image, the main EXE and every DLL alike, with validation of the
   `.pdata` table. No other executable memory is allocated by the bridge.
2. **`__C_specific_handler`: covered.** It is forwarded to the host
   implementation in the source. Correct runtime dispatch for Wine modules
   still needs qualification; this forwarding alone does not prove it.
3. **Implicit TLS is emulated incorrectly.** `RuntimeImage::NotifyTls`
   stores a `TlsAlloc` index into the image's `AddressOfIndex` and puts the
   data block in that dynamic TLS slot. Compiled `__declspec(thread)` code
   instead reads `gs:[0x58]` (`ThreadLocalStoragePointer`) indexed by
   `_tls_index`, which is a different array. The 53-file XCDM audit set has
   no TLS directories; this does not qualify the complete runtime. Reject
   TLS directories explicitly or
   implement a real `ThreadLocalStoragePointer` array.
4. **Last error.** Bridge shims call the host `::SetLastError`. Wine modules
   read `LastErrorValue` straight from the guest TEB. Every bridge-owned
   function reachable from a Wine-profile module (the ntdll/win32u seam)
   must set the error with `xcdm::guest::SetGuestLastError`.
5. **Thread lifecycle hooks still required.** The seam must call
   `GuestProcess::AttachCurrentThread` before guest code runs on a thread
   (process start, guest `CreateThread` trampolines, callbacks into guest
   code from host threads). Its future `ntdll!NtCurrentTeb` provider must
   reject an unattached context and never fall back to the host TEB.
6. **NLS tables still required.** The seam must map `C_1252.NLS`, `C_437.NLS`
   and `l_intl.nls` and pass them through `ProcessConfig` before Wine attaches.
   Wine kernelbase initializes code pages from those PEB fields.

## Build requirements for the XCDM Wine PE build

- Apply `patches/wine/0001-xcdm-guest-ntcurrentteb-redirect.patch`.
- PE compiler: clang >= 19, MSVC target (`x86_64-windows`), e.g.
  `--with-mingw=clang` with an llvm-mingw toolchain.
- PE flags: `-DXCDM_GUEST` (for example `CROSSCFLAGS="-O2 -DXCDM_GUEST"`).
  Check that `-fasync-exceptions` was accepted by configure.
- Gate the output with `tools/audit_wine_guest.py binary ... --wine-build-dir`.

### Admission requirements still outside the environment helper

The environment helper is not integrated into Wine execution. Its tests and
record-layout qualification do not admit any Wine PE into Win32Bridge.
Before initialization, the seam must provide a live owned heap and validated
NLS mappings; the helper currently accepts their raw configured values,
including zero. The seam must also qualify callback-table storage and target
addresses and retain their mapped images for every callback.

TLS bitmap storage is process-wide and TEB slots are per thread. The custom
`RtlAcquirePebLock`/`RtlReleasePebLock`, `ThreadZeroTlsCell` behavior across
threads, expansion-slot cleanup, and guest FLS teardown/callback behavior
must still be implemented and tested together. Initial attachment now rejects
missing or invalid stack bounds; a failed later refresh preserves the last
qualified bounds. No loader gate is removed by these changes.

## Validation

| Check | Where | Result |
| --- | --- | --- |
| Updated `guest_environment_tests` | This workspace, Linux GCC 15, ASan/UBSan | 107/107 |
| Updated `wine_guest_environment_contract` | Windows 2022 runner, MSVC x64, CI 41454321443 | Passed; includes Windows thread/fiber cleanup fixtures |
| Independent compiled Wine ABI comparison | This workspace, Linux GCC 15, syntax-only | 596 assertions passed; deliberate one-byte mismatch rejected |
| Earlier Windows-code-path tests | Collaborator-reported mingw build under Wine 9 on Linux | 102/102; predates the cleanup fixes |
| `verify_guest_layout.py` vs pinned `winternl.h` comments | Linux | 187 fields, 0 mismatches; superseded for independent ABI evidence by the compiled comparison |
| Registry fixture tests | This workspace, Linux | Passed; standalone windres PE fixture skipped |
| Actual compiled registry resources and generated INF | This workspace, Linux, offline only | ExplorerBrowser CLSID check passed; partial seed, 719 keys and 6 warnings |
| Corrected binary-audit fixtures | This workspace, Linux clang 23 | 25/25; optional MinGW CRT/TLS fixture skipped |
| Full repository CTest with local audit dependencies | This workspace, Linux, ASan/UBSan | 13/13; optional fixture cases noted above remain skipped |
| Full repository Windows CTest | Windows 2022 runner, MSVC x64, CI 41454321443 | 11 executed suites passed, 1 audit suite skipped; 12 registered |

The earlier Windows result was reported from running a mingw build under Wine
on Linux; it is not an MSVC, real-Windows or Xbox result. The independent ABI
comparison and its limitations are in
[`../desktop/wine-explorer/guest-abi/README.md`](../desktop/wine-explorer/guest-abi/README.md).
Nothing here has executed a Wine module inside Win32Bridge yet.
The Windows host checks and UWP build passed at source
`3e42c5f2a9bdc50d0cf878aff375b497e2936435` in
[CI run 41454321443](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41454321443).
The audit suite skipped there because its optional cross-tool/dependency
environment was absent; its executed fixture proof is the separate Linux run.
