# UWP data-memory adapter

This is a small, real Windows API port, not a replacement guest kernel or a working Wine backend. It implements a data-memory adapter and a host-service subset. The memory adapter supplies the eight memory methods of `Platform` from Boxedwine64 revision `58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4` using UWP-available memory APIs:

- `alloc64kBlock` and `reserveNativeMemory64k`: checked 64 KiB block sizes and `VirtualAllocFromApp`.
- `allocateNativeMemory` and `commitNativeMemoryPage`: commit existing reservations with `VirtualAllocFromApp`.
- `updateNativePermission`: `VirtualProtectFromApp`, with guest execute mapped to host read-only data. Guest RWX maps to host RW.
- `releaseNativeMemory`: `VirtualFree(base, 0, MEM_RELEASE)`, releasing the entire original reservation as required on Windows.
- Allocation and permission granularity: real host geometry from `GetSystemInfo`, expressed in upstream guest-page units.

No method creates host executable memory. Requests for `alloc64kBlock(..., true)` fail explicitly, and the upstream-facing adapter follows the existing upstream panic-on-memory-failure contract. The adapter rejects JIT compile macros. Invalid counts, null addresses, unaligned commit/protect addresses and wrapping ranges fail before the OS call. Memory errors retain their real Win32 error code.

## Important boundary

The current guest-x64 `KMemory64` path stores its 4 KiB software pages in `new U8[K64_PAGE_SIZE]`, inside `include/kmemory64.h`. It does **not** use these platform virtual-memory methods, and this port intentionally leaves it unchanged. These methods are used by shared soft-MMU/native-heap infrastructure. Finishing them is useful platform groundwork; it is not proof of Wine64, guest-x64 memory semantics, full interpreter linkage, or Xbox execution.

`UwpPlatformMemory.cpp` supplies genuine upstream `Platform` definitions. Never link it alongside the unchanged memory-method block of `platform/windows/platform.cpp`, because that would define the same symbols twice. Remaining platform services still need separate UWP qualification, including file and directory operations, sockets, timers and threading integration. This adapter has no process-launch, syscall, Linux-kernel, graphics or presenter stubs.

## Build and checks

Portable policy checks (no Windows API calls):

```sh
cmake -S backend/uwp -B build/uwp-memory-policy
cmake --build build/uwp-memory-policy
ctest --test-dir build/uwp-memory-policy --output-on-failure
```

Windows SDK checks, from a Windows x64 build environment with the pristine full source checkout already staged:

```powershell
cmake -S backend/uwp -B build/uwp-memory -A x64 `
  -DBOXEDWINE_SOURCE="$PWD/external/Boxedwine64-full"
cmake --build build/uwp-memory --config Release
ctest --test-dir build/uwp-memory -C Release --output-on-failure
```

Targets:

- `protondesktop_uwp_memory`: the real `VirtualAllocFromApp` / `VirtualProtectFromApp` / `VirtualFree` implementation, with `WINAPI_FAMILY_APP` headers and `WindowsApp.lib`.
- `protondesktop_uwp_platform_memory`: object-only compile proof against the exact pinned upstream declarations. The source checkout is checked for revision and tracked modifications. It is not a linked emulator target.
- `protondesktop_uwp_memory_tests`: 23 portable policy assertions; on Windows it additionally executes real reserve, commit, zero initialization, byte preservation, query, protection transitions and release, and checks that guest execute never grants native execute.

The Windows test is a console diagnostic compiled with the UWP API family. A passing desktop test proves those calls on the tested Windows host, not app-container or physical Xbox behavior. No physical Xbox result is recorded by this component.

The root project may register these tests using `add_subdirectory(backend/uwp)` after `include(CTest)`.

## API references

- [VirtualAllocFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualallocfromapp): reserve/commit, no executable allocations.
- [VirtualProtectFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp): change committed-page protection.
- [VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree): full reservation release requires the original base and size zero.

## Host clocks, timer, CPU count and directory enumeration

`UwpHostServices.h/.cpp` adds actual host services; `UwpPlatformHost.cpp` supplies the corresponding pinned upstream definitions:

- `Platform::init` / `startMicroCounter` / `getMicroCounter`: QPC frequency, an atomic epoch and overflow-checked integer conversion to microseconds. Unsupported frequency/conversion ranges fail explicitly.
- `getSystemTimeAsMicroSeconds`: real FILETIME converted to Unix microseconds, with checked epoch subtraction.
- `nanoSleep`: a private relative waitable timer in 100 ns units, rounded upward without overflow; zero duration yields with `Sleep(0)`. It does not spin or truncate the request into a DWORD millisecond timeout. Actual precision depends on the host scheduler/hardware.
- `getCpuCount`: real `GetSystemInfo` count, with no invented fallback processor.
- `listNodes`: strict UTF-8/UTF-16 conversion, `GetFileAttributesExFromAppW`, `FindFirstFileExFromAppW`, `FindNextFileW` and `FindClose`. Filenames and directory attributes come from the OS. Missing/inaccessible paths and invalid UTF-8 fail explicitly at the low-level API. The upstream void `listNodes` wrapper returns normally on those failures, leaves the caller's existing results unchanged, and retains the actual Win32 error through local temporary cleanup. Ordinary filesystem errors do not terminate the application. Caller wildcards and extended-length `\\?\` paths are deliberately rejected pending a path-normalization policy. Native enumeration results replace the temporary snapshot only on success; the upstream wrapper appends that snapshot to its caller's results.

The `protondesktop_uwp_host_services` target compiles and links the Windows API implementation with `WINAPI_FAMILY_APP`. `protondesktop_uwp_platform_host` is an additional **object-only** compile target against the pinned declarations, and intentionally does not define remaining Platform methods. Do not link either adapter beside duplicate original Windows Platform definitions.

`protondesktop_uwp_host_tests` contains portable FILETIME/QPC/relative-timer boundary assertions. On Windows it additionally runs QPC/wallclock/timer/CPU APIs, checks strict Unicode conversion, creates its own temporary directory/files via app-aware APIs, enumerates an actual Greek filename, and tests missing/file/wildcard failures without partial listing results. The scratch parent is passed by CTest. A successful desktop test remains a Windows host result, not an app-container or Xbox result.

These are **not** rootfs file read/write/stat implementations or guest filesystem permission semantics. The rest of the existing Windows platform, CRT filesystem, socket/process emulation and rendering still need separate work. In particular, native socket pairs, `/proc/stat`, CPU-frequency telemetry, desktop file launching, JIT code/cache functions and thread-affinity policies have no replacement definitions here. The waitable timer is synchronous and has no backend-stop cancellation integration yet; it must not be placed on the UI thread. No complete UWP interpreter or Wine Desktop is claimed.

Additional API references:

- [FindFirstFileExFromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-findfirstfileexfromappw)
- [QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)
- [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [SetWaitableTimerEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex)

### Directory failure contract

The low-level `EnumerateDirectory` returns `false` and sets the real Win32 error for ordinary missing/denied paths; an existing empty directory is a successful empty snapshot. Its failed enumeration never replaces a previous result vector. The upstream-facing `Platform::listNodes` has no result status return, so it preserves the pinned upstream contract: failed listings append nothing and return normally. It does not convert a failed OS listing into a successful guest syscall or terminate the runtime. The caller can inspect `GetLastError` immediately after the wrapper call.

The Windows host-service tests exercise the real low-level missing-path, file-as-directory and invalid-wildcard failure paths while preserving existing snapshots. They do not link and execute the upstream `BString`/`Platform::listNodes` wrapper; that wrapper currently has object-compile qualification only. No denied-access app-container/Xbox test has been performed.
