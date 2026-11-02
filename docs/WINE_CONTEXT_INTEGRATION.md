# Guest loader/thread context integration

This change links the isolated guest environment to the strict native bridge
loader. It does not admit a Wine DLL, execute an arbitrary PE, or provide the
Wine window/GDI backend.

## Entry and lifetime

`runtime/GuestExecution.h/.cpp` owns a `GuestProcess` through a shared execution
context. `GuestThreadScope` attaches before entry and keeps the process alive.
Only the outer scope that created the binding detaches it. Nested callbacks
share the same TEB; an existing manual attachment remains its owner's
responsibility. Failed entry rolls back a newly created attachment. Cross
process/fiber entry and out-of-order or cross-thread destruction are rejected.

`RequestStop` prevents new external entries; existing nested entries may finish.
`WaitForIdle` drains entered scopes and rejects a self-wait. It does not count
manual attachments or drain DLL/image references, queued callback records or
host threads. This context pins the process only.

The new bridge overlay is applied after the x86 and module-ownership overlays.
An owned strict `GuestModuleLoader` carries the context. `GuestModuleScope`
pins that loader, enters the guest thread scope, and only then publishes the
current loader. It restores the previous loader and detaches in the correct
destruction order. Missing context, another process's loader and a downgrade
to Legacy all fail closed, including when a host fiber switch hides the
current guest TEB.

Four Bridge-owned ntdll imports now use that context: `NtCurrentTeb`,
`RtlGetCurrentPeb`, `RtlGetLastWin32Error` and `RtlSetLastWin32Error`.
Static and dynamic strict resolution reach the same providers. An absent
owned entry raises a noncontinuable fault; there is no host TEB fallback.
The kernel32 last-error adapters use the guest TEB during an active context.
This does not yet update every adapter's error conventions or TLS semantics.

Strict `GuestRuntime::Run` now establishes the real loader/thread scope before
returning the unconditional execution-block diagnosis. It does not bind/init
Wine or call the PE entry point. Strict child-process and asynchronous-thread
launch explicitly reject until full code lifetime and callback ownership are
qualified. Existing Legacy execution remains unchanged.

## Native integration diagnostic

`bridge/GuestContextProbe.cpp` is compiled into the actual bridge DLL, separate
from the WinRT class ABI. Its bounded C export exercises the actual strict
loader/scope, nested state, direct and dynamic TEB imports, guest last error,
missing-context faults, cross-loader rejection and a joined host worker. The
worker must retain its loader/context until exit. A self-authored return42 PE
is prepared through the strict runtime solely to verify that Run stays blocked;
its entry point is not executed. No Wine file is loaded.

Version 1.0.50.0 adds a private app-local `guestContext: true` smoke option. It
loads the packaged bridge and saves `xwindows-guest-context-smoke.json` to
LocalState. The JSON explicitly keeps guest/Wine execution, heap/NLS readiness
and asynchronous image-lifetime qualification false. It runs only when an
explicit local test configuration requests it; there is no website/RPC action.

## Validation and remaining work

The portable helper passed 59 ASan/UBSan checks: nesting/manual attachment,
cross-process rejection, thread error/TLS isolation, stopping/drain behavior,
last-owner lifetime, C++ unwind and allocation/stack-query rollback. A negative
wrong-thread destruction test aborts as required. Windows-specific fiber tests
are part of the host suite; Windows compilation and Xbox diagnostic results
must be recorded separately.

The following remain prerequisites before any Wine initialization:

- Own the runtime, main image, resources and all DLL code for every worker,
  callback and unwind frame. Current Legacy detached threads still borrow raw
  runtime/image pointers; a process pin alone does not repair that.
- Capture originating process and code ownership in async records, or dispatch
  onto the correct guest thread. CoreWindow handlers can call hooks and
  `WM_NCHITTEST` directly; attaching only Run/CreateThread is insufficient.
- Cover WndProc, hooks, dialogs, ListView comparators, COM calls, APC/I/O/FLS,
  resource/font/locale enumeration, threadpool and exception callbacks.
- Replace or checkpoint the current `longjmp` guest-exit path. It can skip
  nested C++ scope destructors; forced exits/fiber deletion that bypass unwind
  are unsupported by this scope helper.
- Provide a live guest heap, validated owned NLS mappings, consistent TLS/FLS,
  loader data and registry initialization. Zero/raw configuration values in a
  scope diagnostic do not establish those services.

The corrected 53-file binary audit and its conservative CRT-data limitations
remain recorded in [`WINE_XCDM_BUILD.md`](WINE_XCDM_BUILD.md). All executable
Wine load and execution gates remain closed.
