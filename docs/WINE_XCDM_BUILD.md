# XCDM Wine build and corrected audit

The native Wine work has build evidence, not a working Explorer. No Wine PE
has been initialized or executed inside Win32Bridge, on Windows or Xbox.
The strict loader's executable-load and execution gates remain unconditional.
The installed desktop is the separately tested XWindows OS 1.0.49.0 build.

## Build evidence, 2026-11-02

`desktop/wine-explorer/pe-build/build_pe.py --variant xcdm` applied the exact
guest-accessor patch to a separate verified extraction of Wine
`1c78596625336a73dbfc035dbbcf0dbdd95b06eb`. It built **61 AMD64 PE files**, with
zero missing dependencies among the parsed ordinary/delay imports and exported
forwarders. Dynamic runtime dependencies and API behavior remain unqualified.

The compiler proof uses the actual configured flags and pinned `excpt.h`:
clang 23, target `x86_64-windows`, `_MSC_VER=1933`, `XCDM_GUEST=1`,
`USE_COMPILER_EXCEPTIONS` defined, and `-fasync-exceptions` enabled. This proves
compiler/header selection without proving OS exception dispatch or executing
guest code. Source archive, patch, compiler hashes and commands are recorded.
Reused source/tool bytes are explicitly distinguished from fresh extraction;
bit-for-bit reproducibility is not claimed.

Evidence is in the ignored `artifacts/xcdm-wine-pe-build/` directory:
`closure.json`, `build-provenance.json`, `compiler-seh-proof.json`,
`guest-selection.json`, `guest-admission-audit.json`,
`guest-admission-summary.json` and `logs/`.

## Binary audit outcome

The original `tools/audit_wine_guest.py binary` actually inspected
**53/53 files**, returned **1**, and reported **49 problems**: GS candidates in
48 modules and an additional SEH finding in msvcrt. All 53 import
`ntdll!NtCurrentTeb`; none has a TLS directory. The reported ordinary/delay
import union is 505 names from ntdll and 429 from win32u. These are conservative
requirements for this audit superset, not counts of implemented or reachable
APIs and not a startup execution trace.

The audit excludes the built Wine ntdll/win32u providers and six Unix-side
modules: bcrypt, crypt32, dnsapi, netapi32, secur32 and ws2_32. It deliberately
includes kernel32/kernelbase for static comparison. Both are Bridge-owned in
the strict loader policy. This input set is an audit superset, not a list of
admitted Wine guests; unknown modules still fail closed in the loader.

Two audit defects are independently reproduced:

1. `gs_sites()` disassembles starting at every byte `0x65`, including bytes
   inside other instructions. In advapi32, RVA `0x1094` is
   `48 8d 0d 65 ff 01 00` (`lea rcx,[rip+0x1ff65]`). Its displacement byte at
   `0x1097` is incorrectly reported as a GS instruction. In Explorer, RVA
   `0x1dbf` is `75 65` (`jne 0x1e26`); the branch displacement at `0x1dc0` is
   likewise treated as a GS prefix for the following instruction.
2. msvcrt's unwind records point to `local:0x17e00`. Its export table identifies
   that same RVA as `__C_specific_handler`, but the tool resolves only imported
   handler thunks and reports the local handler as absent. Source-level
   `__TRY` counts also include files/branches that may not be built for x64.

The original failed audit is retained in
`before-instruction-fix-guest-admission-audit.json` and its summary. The
defects were corrected locally and the tool rerun against the same binaries.

## Corrections and rerun

The scanner now decodes executable sections sequentially. Operand bytes are
not separate starts; file-alignment padding is excluded, while unreadable or
undecodable executable bytes are recorded as inconclusive and fail the check.
Local exported handlers are resolved by RVA, including the five msvcrt unwind
records targeting its own `__C_specific_handler`. Unwind arrays and bounded
chains are validated; malformed/truncated/cyclic metadata fails explicitly.

Regression fixtures reproduce both operand-byte false positives, reject a
genuine GS access, recognize imported/local/chained SEH handlers and reject
malformed metadata. **25/25 checks passed** with clang 23 and build-local
pefile/capstone. The optional MinGW CRT/TLS case was skipped because MinGW is
absent. The full Linux ASan/UBSan CTest run passed **13/13** with those local
dependencies; the independent windres PE-resource fixture also remains skipped.

The corrected full audit inspected **53/53 files**, found **zero decoded GS
instructions and zero TLS directories**, and reported no problem for 51.
It still returned **1**: msvcrt has 33 executable bytes and ucrtbase has 34
that the linear sweep could not decode. Both scans are **inconclusive**.
The driver preserves the failed status and full module coverage. These are
static observations: no control-flow recovery, embedded-data distinction or
alternate instruction-boundary proof is claimed. Source `__TRY` counts are
conservative; absent handlers are no longer asserted to prove setjmp use.

Native ntdll/win32u providers, NLS/heap services, thread/callback lifecycle,
guest last error and registry integration remain separate runtime work. No
execution gate was loosened, and no Wine DLL was executed.

## Offline registry evidence

The trusted host build generated `loader/wine.inf`; no PE was executed. The
registry tool then parsed it and the available compiled `WINE_REGISTRY`
resources into `registry-seed.json`: **719 keys**, five modules replayed, six
warnings, and a passing ExplorerBrowser InprocServer32 check pointing to guest
`C:\windows\system32\shell32.dll`. Twenty-seven INF registration modules are
absent from this build set, and custom registration work was not executed.
This is partial offline seed evidence, not a complete boot registry or proof
that COM activation works.
