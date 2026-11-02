# Native Wine module ownership foundation

This is a bounded loader foundation, not a working Wine desktop. The production default remains `Legacy`. The experimental `StrictWine` profile is selected only through a private C++ constructor; the desktop UI does not enable it.

## One provider per module

`runtime/ModuleOwnership.h/.cpp` defines an immutable policy for each guest runtime and its child processes. Strict mode selects the provider before resolving a symbol:

| Provider | Modules |
| --- | --- |
| Bridge | `ntdll`, `win32u`, `kernel32`, `kernelbase` |
| Wine | `user32`, `gdi32`, `gdi32full`, `comctl32`, `comdlg32`, `shell32`, `shlwapi`, `shcore`, `ole32`, `oleaut32`, `combase`, `rpcrt4`, `advapi32`, `msvcrt`, `ucrtbase`, `msvcp_win`, `version`, `imm32`, `winspool.drv` |
| Reject | Every unlisted module or contract |

Names are case-normalized bare ASCII filenames. Missing `.dll` extensions are accepted; paths, ambiguous extensions, embedded NULs and other unsafe spellings are rejected. Fourteen exact API-set aliases are listed in `ModuleOwnership.cpp`; unknown contracts are rejected rather than guessed from substrings. Known aliases share their target module's owner and cache identity.

Wine modules have one proposed location: the app-owned guest `C:\Windows\System32`. Strict resolution does not search USB, caller-selected directories or the host's System32. This provider list is an experimental contract, not proof that the bridge implements every required export or that the Wine DLLs are compatible.

## Integration and execution gates

`patches/Win32Bridge-wine-ownership.patch` applies to a disposable upstream source copy **after** `Win32Bridge-x86-startup.patch`. It routes main-image and dependency imports through the shared ownership decision. Strict preparation does not prebind through the raw shim resolver. Dynamic module handles, `LoadLibrary` and `GetProcAddress` respect the same owner, including rejection of forged legacy bridge tokens for Wine-owned modules. Forwarders reclassify their target module; ordinary delay-load helpers reach the same dynamic API policy. A missing export reports its owner and fails without trying the other provider.

Strict runtime execution and executable DLL loading are unconditionally blocked. There is no configuration switch to bypass this gate. Resource-only mappings remain inert. The loader/thread scopes now carry an owned guest process context, while native providers, complete callback/image lifetime and TLS qualification remain incomplete. See [`WINE_CONTEXT_INTEGRATION.md`](WINE_CONTEXT_INTEGRATION.md). Existing unwind registration in `RuntimeImage` is unchanged.

Two existing lifetime constraints are particularly important before removing the gates:

- An absent `CurrentGuestModuleLoader()` currently selects Legacy behavior in the bridge's synthetic-module helpers. Every guest thread and callback must establish its captured strict loader scope before calling a bridge API.
- Detached guest threads borrow the runtime and mapped image. Shutdown must keep that state alive until all guest execution and callbacks finish, before unloading DLLs or releasing image memory.

## Validation and proof limits

`tests/ModuleOwnershipTests.cpp` exercises **59 portable policy/router invariants**, including canonical names, rejected names and aliases, provider selection, missing exports, legacy behavior, independent session profiles and nested forwarder ownership. The shared routing primitive is the one used by the bridge overlay. These checks passed locally with AddressSanitizer and UndefinedBehaviorSanitizer. The overlay also passed a clean application check after the existing x86 overlay.

The portable checks do not execute the C++/CX loader, real PE delay-load helpers, Wine DLLs or Explorer. Windows compilation and physical Xbox execution are separate validation stages. Passing these checks does not qualify the guest environment, establish COM/shell functionality or demonstrate any game compatibility.
