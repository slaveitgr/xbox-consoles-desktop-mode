# Guest environment audit before implementation

This tool collects review evidence before any `XCDM_GUEST` / `xcdm_guest_teb()`
header, PEB/TEB structure or live runtime change. It reads source and optional
PE64 files. It never downloads source, runs upstream build scripts, loads a DLL
or executes a guest program.

```sh
python3 desktop/wine-explorer/environment-audit/audit_environment.py \
  --source-root /path/to/pinned-wine-source \
  --source-pin 1c78596625336a73dbfc035dbbcf0dbdd95b06eb \
  --output /path/to/environment-audit.json
```

Default seeds cover Explorer, shell32, comctl32, ole32, shlwapi, user32, gdi32,
kernel32, kernelbase, ntdll and win32u. Repeat `--module` to choose another seed
set. The scanner follows literal/local-variable `IMPORTS`, `DELAYIMPORTS` and
`PARENTSRC` references and scans those module directories plus `include/`.
Eager and delayed reachability remain separate. Unresolved build variables and
dependencies remain in the report; this is not a complete generated PE import
closure. Module tests are excluded unless `--include-tests` is supplied.

Every finding records a relative file path, line, snippet, category, basic
code/comment/string context and source role. `/unix/` and `#pragma makedep unix`
files are classified separately from PE source candidates; shared headers are
also separate. Architecture/build conditions are not evaluated. Matches inside
assembly strings matter, while documentation and inactive branches can be false
positives. Findings include direct GS/FS forms, intrinsics/builtins, NtCurrentTeb
references/definitions, RtlGetCurrentPeb, compiler TLS declarations/macros and
static TLS indications. Dynamic `TlsAlloc`/`FlsAlloc` APIs are a separate category.
Evidence counts do not represent compiled blockers or numbers of fixes.

For existing outputs, repeat `--binary /path/to/module.dll` or `.exe`. The tool
accepts bounded AMD64 PE32+ images, reports `.tls` sections and TLS directory
fields, resolves `AddressOfIndex`/callback VAs, and distinguishes file-backed
data from virtual zero-fill. Optional GNU `objdump` disassembly records GS/FS
instruction evidence; use `--objdump` to choose its executable. Disassembly is
not execution or exhaustive reachability analysis.

**No binary inputs means unknown binary coverage.** A missing disassembler or
invalid PE likewise produces an explicit unknown result and a null instruction
count. A supplied executable without TLS metadata does not prove that the other
Wine DLLs lack compiler TLS or direct GS/FS accesses. The tool never asserts
that the full selected DLL closure was supplied or that a TEB accessor patch
is sufficient. Source identity records the requested pin and available adjacent
provenance without claiming independently verified source authenticity.

Self-authored source and PE metadata fixtures:

```sh
python3 -m unittest discover -s desktop/wine-explorer/environment-audit/tests -v
```
