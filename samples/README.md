# Self-authored runtime diagnostic

`make_return42.py` deterministically creates `return42.exe`, a 1024-byte PE32+
AMD64 image. `Return42Fixture.cpp` embeds those same bytes for the native probe.
It contains one read/execute `.text` section and six bytes of position-independent
machine code: `mov eax, 42; ret`. No third-party binary or compiler runtime is
included.

Regenerate both fixtures from the repository root:

```sh
python3 samples/make_return42.py --cpp-output samples/Return42Fixture.cpp
```

The entry point has the diagnostic contract `std::uint32_t (*)()`; the probe calls
it in its own process and requires the result 42. This is **not** a Windows CRT
startup contract. Do not use double-clicking the file as a supported test.

The runtime only executes this embedded fixture. It does not accept a game or
arbitrary EXE path. The parser rejects imports, relocations, TLS, CLR, exception
metadata, other data directories, overlapping ranges, and writable/executable
sections. Even an accepted image is not proof that its code is position
independent or compatible with UWP.

On x64 MSVC Windows builds the probes request writable memory, copy the code,
change code pages to read/execute, flush the instruction cache, invoke under an
isolated SEH boundary, and release the allocation. The PE probe additionally maps
headers and sections separately; headers become read-only and gaps inaccessible.
They return real API error codes or SEH exception codes when an operation fails.
No page is requested writable and executable at the same time.

Linux and non-x64 hosts return `Unsupported` for execution probes. Portable parser
tests, a successful Windows build, a desktop UWP run, and a physical Xbox run are
separate results. Only the last can establish that this diagnostic runs on the
console; none establishes compatibility with Windows games.
