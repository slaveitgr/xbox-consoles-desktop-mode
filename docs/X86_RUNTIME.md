# Integer x86 / Win32 runtime bootstrap

Version 0.4 adds execution of an embedded, self-authored **PE32/i386** program.
This is distinct from the existing PE32+ native return-42 probe and USB file
inspection. Version 0.5 additionally passes the selected `gta_sa.exe` to a
bounded startup-tracing profile, described below; it does not implement gameplay.

## Execution path

1. `Pe32Image` validates the initial loader profile and maps headers and sections
   into a guest byte vector at their virtual addresses.
2. Each supported 32-bit IAT entry receives a synthetic guest gate address.
   No native x64 function pointer is truncated or exposed to guest code.
3. `X86Execution` interprets the program through the pinned libx86emu core.
   Reads, writes and instruction fetches use checked guest-memory callbacks.
4. Reaching a gate invokes a typed host handler. It reads 32-bit stdcall
   arguments and return addresses, sets EAX, and restores guest EIP/ESP.
5. Guest `ExitProcess` stops the interpreter; it never exits the UWP host.

The fixture calls `SetLastError(0x1234)`, checks `GetLastError()`, emits
`x86 Win32 host calls passed` through `OutputDebugStringA`, and exits 0.
A mismatched result exits 1. Success requires the exact expected call trace,
error value, debug message and exit code, not just an accepted PE header.

The test starts automatically when the UWP app launches. **Run x86 test** or
keyboard **T** repeats it. `x86-runtime.txt` is saved to app LocalState with the
device family, OS, package version, engine stop reason and trace. The Game page
shows a separate execution result alongside USB metadata.

## Current limits

- The interpreter implements regular integer x86 and basic protected mode.
  **FPU, MMX and SSE are absent.** This engine establishes the execution and
  host-call interfaces; it is not a San Andreas performance solution.
- The only Win32 calls are the four above. Files, threads, callbacks, audio,
  DirectInput, Direct3D 9 and general DLL loading are not implemented.
- The initial loader requires fixed-base PE32 executables, 4-KiB section and
  512-byte file alignment. It rejects DLLs, TLS, relocations, CLR and
  writable/executable guest sections. The strict fixture profile rejects delay
  imports; the startup profile accepts bounded RVA/VA delay-import descriptors.
  This is a limited loader profile, without dependency loading or DLL initialization.
- Guest memory is not cast to native pointers. The guest image, read-only
  sections, executable sections and non-executable stack are checked separately.
  No JIT or executable host memory is used by this interpreter path.
- Instruction and memory-operation budgets also bound loops and REP work.
  Hardware I/O and privileged MSR access are disabled. This is an experimental
  compatibility component, not a security boundary for arbitrary untrusted code.

## Validation

`tests/Pe32GuestTests.cpp` checks mapping bounds and actual guest success/failure
paths. The existing native probes and metadata parser remain separate tests.
Run `python3 samples/make_x86_win32.py --check` to verify fixture reproducibility.
Windows packaging and physical Xbox results are recorded separately in
`VALIDATION.md`; a Linux interpreter pass alone is not an Xbox pass.

## Bounded supplied-image startup trace

**Trace game startup** / **G** reopens the saved folder grant without a picker,
reads at most 64 MiB and interprets up to 50,000 instructions. It also runs at
launch if a folder grant exists. `game-startup.txt` includes the image hash,
source, entry address, instruction count, section layout and actual stop reason.

A virtual single-thread TEB/PEB supplies the initial SEH sentinel, stack bounds,
TEB self pointer, emulated process/thread IDs, image base and per-thread last
error. FS points to this virtual region, never to the host's TEB. Unsupported
imports are bound to stop gates instead of being reported as implemented.
Normal and delayed imports share bounded counts. Eager delay gates bypass the
linker's lazy resolution helper for this diagnostic profile.

The four implemented APIs retain their exact guest behavior. All other APIs,
unsupported CPU instructions, invalid memory accesses and exhausted budgets stop
the trace. An explicit guest exit of zero still does not establish compatibility.
No game files are copied or modified by the trace.

The self-authored smoke report also records a separate native low-address
capability diagnostic: simultaneous exact RW image, stack and TEB-container
reservations needed by a potential DBT backend. These are allocated, checked
and released. Its result does not determine the integer smoke-test outcome.

The earlier physical USB test on One X read the user's PE32 x86 San Andreas,
including its delayed Direct3D 9, DirectInput and DirectSound imports. That test
proves folder access and metadata inspection, not execution by this runtime.
