# UWP data-memory adapter

This is a small, real Windows API port, not a replacement guest kernel or a working Wine backend. It implements the eight memory methods of `Platform` from Boxedwine64 revision `58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4` using UWP-available memory APIs:

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
