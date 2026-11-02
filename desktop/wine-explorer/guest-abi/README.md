# Wine guest record ABI qualification

This syntax-only check compares `runtime/GuestEnvironment.h` against **actual
compiled declarations** from pinned Wine commit
`1c78596625336a73dbfc035dbbcf0dbdd95b06eb`. It does not use Wine's offset
comments as expected values. No Wine object is linked, loaded or executed.

```sh
python3 desktop/wine-explorer/guest-abi/qualify_abi.py \
  --source-archive artifacts/reference/wine-1c78596625336a73dbfc035dbbcf0dbdd95b06eb/source.tar.gz \
  --output /tmp/xwindows-guest-abi-qualification
```

The runner first verifies the complete source archive SHA-256 and extracts its
include tree into a fresh temporary directory. It verifies the locked hashes
of `winternl.h`, `winnt.h` and `ntgdi.h`; mutable Wine build trees are not used.
Wine headers retain their LGPL notices in that temporary tree. This folder
contains self-authored checks and field mappings, not copied Wine types.

The host compiler is invoked with `-std=c++17 -m64 -fshort-wchar
-DWINE_UNIX_LIB -D_WIN64 -fsyntax-only`. The report records its resolved path,
SHA-256, version, arguments and return code. `layout-map.json` explicitly maps
field names and supplies no expected byte offsets. The compiler calculates
`sizeof`, `alignof`, `offsetof` and field widths on both sides.

The saved Linux GCC 15 result passed 596 assertions:

- 15 record types: size, alignment and standard layout.
- 187 addressable TEB/PEB fields: offset and field size.
- 82 supporting fields, including process parameters, loader data, GDI,
  NT_TIB, strings, lists, bitmaps and activation context state.
- Three non-addressable PEB bitfield storage boundaries and widths.
- Pointer/WCHAR widths and the complete GDI shared table size/count.

A negative control deliberately shifts one field offset by one byte. It must
fail compilation with its unique assertion diagnostic, or qualification fails.
The runner writes `qualification.cpp`, compiler logs, the negative control and
`qualification.json`; exit 0 means layout qualified, 1 means a comparison or
negative-control failure, and 2 means input/tool failure. An input/tool failure
overwrites any previous success report in the output directory.

Three PEB words correspond to C++ bitfields, whose addresses cannot be taken.
Their surrounding compiler-calculated storage boundaries are checked, but bit
order is not independently qualified. This Linux compiler check is not an
MSVC/UWP build or Xbox hardware result. It does not establish pointer lifetime,
heap/NLS semantics, callbacks, loader behavior, SEH, TLS/FLS, or syscall support.
Every report keeps `guest_execution_admitted: false` and `NOT SAFE TO EXECUTE`.
