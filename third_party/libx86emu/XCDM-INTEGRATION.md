This source snapshot comes from https://github.com/wfeldt/libx86emu at commit
`ce81129c57fdfbf690bebc210a8db97d926cc5e7`. `UPSTREAM.sha256.json` records the
original downloaded files' SHA-256 values, before the local changes below.
The original copyright and permissive license remain in each source file,
`LICENSE` and `LICENSE_INFO`.

Local changes for the Xbox UWP interpreter host:

- GNU attributes have a portable MSVC spelling/empty alternative.
- Real host TSC and physical I/O support are disabled on every build. The
  adapter intercepts all guest memory and port accesses and supplies CPUID.
- Initial emulator/memory allocations handle failure before dereferencing.
- REP string/I/O loops observe the core's halted state, so callback faults
  and the adapter's finite memory-operation budget stop long REP operations.
- The opcode table rejects upstream's partial SSE move/logical operations as
  unsupported, matching this host's integer-only CPU profile. Other missing
  FPU/MMX/SSE instructions already raise the core's invalid-opcode exception.

The C files `api.c`, `decode.c`, `mem.c`, `ops.c`, `ops2.c`, `prim_ops.c` build as
a separate C library. They do not need an instruction-set compiler flag, JIT,
executable host pages, native guest pointer mappings or a Unix runtime.
`runtime/X86Execution.cpp` provides the C++17 callback adapter.

The upstream core targets regular 16/32-bit x86 and basic protected mode. It
does not supply a complete FPU/MMX/SSE implementation, Win32, Direct3D, or a
game runtime. This integration advertises an integer i386 guest CPU profile.
The initial use is an import-aware, self-authored PE32 runtime smoke test.
