# Real Wine Explorer integration

The requested target is Wine Explorer itself. The existing HTML Files app is
not Wine Explorer. This directory contains pinned upstream source staging, an
actual Wine-source PE64 build target and a self-authored qualification guest.
It does **not** yet provide a working Wine Explorer on Xbox.

## What original MihawkOS uses

MihawkOS runs a native Wine Explorer session with `MIHAWK_DESKTOP=1`.
`programs/explorer/explorer.c` creates `CLSID_ExplorerBrowser` and hosts its real
shell view. Mihawk patches 0983 and 1150–1152 add the Explorer chrome in
`wine/ps5/pw_shell_explorer.h`. The real shell view retains folder enumeration,
selection, clipboard, context menus, history and file operations. The browser
prototype in `prototypes/windows11-shell` is a separate implementation.

Primary pinned sources:

- [MihawkOS architecture](https://github.com/mihawk-99/PS5_MihawkOS/blob/f81a028a668b36f5bcb51b09e66d526979382f1d/docs/ARCHITECTURE.md)
- [Wine Explorer entry and window](https://github.com/ValveSoftware/wine/blob/1c78596625336a73dbfc035dbbcf0dbdd95b06eb/programs/explorer/explorer.c)
- [Wine ExplorerBrowser implementation](https://github.com/ValveSoftware/wine/blob/1c78596625336a73dbfc035dbbcf0dbdd95b06eb/dlls/shell32/ebrowser.c)
- [Wine shell view](https://github.com/ValveSoftware/wine/blob/1c78596625336a73dbfc035dbbcf0dbdd95b06eb/dlls/shell32/shlview.c)
- [Mihawk Explorer source](https://github.com/mihawk-99/PS5_MihawkOS/blob/f81a028a668b36f5bcb51b09e66d526979382f1d/wine/ps5/pw_shell_explorer.h)

## Prepare the actual source

Run from the repository root with Python 3.9 or newer:

```sh
python3 desktop/wine-explorer/prepare_sources.py
python3 desktop/wine-explorer/prepare_sources.py --offline
```

`source-lock.json` pins 64 files (1,943,854 bytes) by repository, commit, size and
SHA-256. The first command retrieves selected Wine source and the exact Mihawk
source, patches, layout and Explorer sprite inputs. The second verifies the
staged files without network access. Existing files with different checksums
cause failure; the tool does not silently replace them. Staging does not run
upstream build scripts. The default ignored output is `staged/`; `--output`
can select another directory.

This is the relevant source selection for porting and dependency review, not a
complete Wine source tree or a standalone build closure. Build-generated
headers, additional dependencies and a platform adapter remain necessary.
Wine's original `COPYING.LIB` and Mihawk's `LICENSE`, `LICENSING.md`, `NOTICE.md`
and theme `COPYING.LIB` are staged with their source. No Wine executable,
commercial game, Microsoft asset or restricted Bubble City wallpaper is copied.
Wine source retains LGPL-2.1-or-later. Mihawk modifications retain GPL-3.0-or-later
and their component notices. A distributed derived build must satisfy those
licenses and provide its corresponding source; the root repository's MIT
license does not replace upstream terms.

## Build the actual native Explorer source unit

On the existing Windows VS2022 desktop C++ build host:

```powershell
pwsh desktop/wine-explorer/build-native.ps1
```

This compiles the pinned `programs/explorer/explorer.c` itself, with its original
Explorer resources and manifest, as `build/native/MihawkWineExplorer.exe`.
The folder window, navigation, address bar, event handling and message loop
remain Wine's code. `native/prepare_native.py` verifies the source checksum and
emits the complete adapted source, a reviewable patch and provenance. It does
not generate another file-manager UI or implement a fake `IExplorerBrowser`.

The added support unit replaces the CRT startup/memory/string subset with
direct Win32 heap calls and small tested helpers. It links a native x64 PE32+
guest with direct SDK imports, no default CRT, no Wine Unix loader, no delay-load
bootstrap and no CPU emulator. Command-line arguments are limited to 259 UTF-16
code units before they reach Wine's `MAX_PATH` parser. File/path text is not
translated; the adapter folds ASCII command-option names only. Its source
patch adds correct COM event `QueryInterface`/`AddRef`, optional ShellWindows
null handling, clean allocation/COM/window failure reporting, terminated address
input and bounded shell-selection messages.

This is the **file-window source unit**. The actual Wine shell provider,
ListView/COM services and desktop/session manager are not linked. `/desktop`
explicitly reports that missing component. On Xbox the direct desktop imports
must be handled by Win32Bridge; building this PE does not make the missing
ExplorerBrowser or shell view available. Its runtime report is written only
through the guest virtual filesystem to `C:\wine-explorer-native-report.txt`.
No script in this directory deploys or runs the Wine executable.

The build performs host adapter contract tests and offline PE/import checks.
The PE gate rejects x86/DLL images, writable/executable sections, TLS/CLR/delay
directories, UCRT/vcruntime/Win32u/other unexpected runtime libraries, and Wine
Unix/delay-loader bootstrap imports. It requires the actual Explorer/adapter
API imports. This qualifies the **build format**, not API behavior or a working
Xbox desktop. Windows compilation and physical execution results must be
reported separately.

The output artifact includes the executable, `native-imports.json`,
`native-build-info.json`, generated source/patch/provenance, and a complete
`source/corresponding/desktop/wine-explorer` tree containing every compiled
source, support/project/build/validation script, source lock and selected
upstream sources/licenses. It contains no Windows SDK headers or libraries.

The first Windows CI build compiled successfully. Independent read-only
inspection of its PE64 artifact recovered **64 imports across 8 DLLs**, including
13 ordinal imports; the corrected dumpbin and GNU objdump inventories agree.
This verifies the build and import inventory only. The guest has not been run
on Windows or Xbox, and its Wine shell provider is still absent.

Portable validation without Wine execution:

```sh
python3 desktop/wine-explorer/prepare_sources.py
python3 -m unittest discover -s desktop/wine-explorer/native/tests -p test_source_tools.py -v
cmake -S desktop/wine-explorer/native/tests -B desktop/wine-explorer/build/host-checks
cmake --build desktop/wine-explorer/build/host-checks
ctest --test-dir desktop/wine-explorer/build/host-checks --output-on-failure
```

## Current bridge blockers

The current pinned x64 Win32Bridge has real virtual storage, window/GDI and some
shell/OLE adapters. Its existing shell namespace is not Wine's namespace.

| Required behavior | Current source evidence |
| --- | --- |
| Activate `CLSID_ExplorerBrowser` | `BridgeCoCreateInstance` in `Bridge/OleShims.cpp` only constructs `CLSID_ShellLink`; other classes return `REGDB_E_CLASSNOTREG`. |
| Obtain `IShellView` from a folder | `FolderCreateViewObject` in `Bridge/Shell32Shims.cpp` returns `E_NOINTERFACE`. |
| Query folder identity and columns | The bridge folder supports `IUnknown`/`IShellFolder`; `IPersistFolder2` and `IShellFolder2` are not exposed. |
| Share item identifiers with Wine | Bridge PIDLs use a private `WB` layout. Wine `ebrowser.c`, `shlview.c` and filesystem folders call `_IL*` helpers that interpret Wine PIDLs. These layouts cannot be mixed. |
| Load Wine shell classes from a private DLL | The bridge COM adapter does not load guest class factories. A factory dispatch path or explicitly linked port is required. |
| Reuse a stock Wine binary | Wine loader/CRT/delayed imports and additional Win32 services are needed beyond the GUI classes. |

The selected source path is:

```text
Wine programs/explorer/explorer.c
  -> CoCreateInstance(CLSID_ExplorerBrowser)
  -> Wine dlls/shell32/ebrowser.c
  -> IShellItem.BindToHandler(IShellFolder)
  -> IShellFolder.CreateViewObject(IShellView)
  -> Wine dlls/shell32/shlview.c
  -> IShellView.CreateViewWindow (real common controls)
  -> Wine filesystem/desktop folders + PIDL helpers + storage APIs
```

### Minimum implementation batches

1. **Browser/view and class factories.** Port `ebrowser.c`, `shlview.c` and their
   interface services (`IShellBrowser`, `ICommDlgBrowser`, `IObjectWithSite`,
   `IFolderView`, `IPersistFolder2`). The shell view must have a real ListView,
   selection and navigation before claiming Explorer support.
2. **One consistent shell namespace.** Port or adapt `shellitem.c`, `pidl.c`,
   `shfldr_fs.c`, `shfldr_desktop.c`, `shfldr_mycomp.c`, enumeration and folder
   helpers. Use Wine PIDLs end to end. Route file access through the bridge's
   authorized virtual filesystem; keep USB/folder grants and write permissions.
3. **Operational shell services.** Provide context-menu/data-object/clipboard
   services (`shlview_cmenu.c`, `shlmenu.c`, `new_menu.c`, `dataobject.c`,
   `clipboard.c`), file operations, icons and change notifications. Support must
   reflect actual operations, including failure and access-denied results.
4. **Mihawk's Explorer window.** Build the actual `explorer.c` with Mihawk's
   patches/style/layout/sprite generation against the qualified shell layer.
   Its standalone file-manager mode can omit desktop/taskbar/session management.
   `IShellWindows` registration/reuse and NamespaceTreeControl can be deferred
   for one `EBO_NOBORDER` window; the source must handle their absence safely.
   This does not replace the Xbox system shell or launch a PS5 payload.

These PE Windows-side components can be source-ported without also porting
PS5's payload loader, wineserver transport or Vulkan driver. That is an
architectural route, **not proof that the present bridge is sufficient**.
They depend on many shared shell helpers and Windows semantics; copying
`explorer.exe` or compiling `explorer.c` alone does not supply them. A Wine PE
module using existing NT desktop services can work on a full Windows desktop;
Xbox UWP does not expose that host shell. The chosen native path must supply
the guest shell/window/storage services through the UWP host and Win32Bridge.

### Next native provider batch

An approved per-session CLSID-to-guest-DLL mapping can route activation through
`GuestModuleLoader` to Wine's `DllGetClassObject`/`IClassFactory`. Loading
`shell32.dll` via `BridgeLoadLibraryW` currently returns a bridge module token;
it does not load a Wine provider. `CoGetClassObject`, `CoRegisterClassObject`
and `CoRevokeClassObject` adapters are absent. Provider objects must retain
their module until all objects are released. Guest COM objects must be released
before dependency code is unloaded; current reset/teardown order needs review
for that change. Never invoke guest callbacks while holding loader/COM locks.
The existing thread-local `CoInitialize` counters are not COM marshaling.

Further required contracts must remain real: Wine's path callback queries
`IPersistFolder2`, and its image-list code uses a COM `IImageList`. The bridge's
opaque `HIMAGELIST` handle cannot be cast to an `IImageList` and released. Adding
only the ExplorerBrowser class would leave these additional crash paths.

## Qualification guest

On the Windows development build host:

```powershell
pwsh desktop/wine-explorer/build-probe.ps1
```

This compiles `ExplorerContractProbe.exe`, an ordinary no-CRT PE32+ x64 guest.
It is intentionally not a UWP app and must not be loaded directly into the UWP
host. A test may open it through the existing Win32Bridge guest launch path.
The build command does not deploy or run it.

The guest checks COM activation as a batch, then the actual folder/browser/view
contract. It reads at most 64 namespace entries, requests no file mutation, and
writes only `C:\explorer-contract-report.txt` in the guest virtual filesystem.
Results also appear in a guest message box. A passed view contract means that a
real view was obtained, with a view window and `IFolderView`; it does not mean
Wine Explorer itself has been compiled or tested. The current bridge source
predicts class-not-registered and no-view results. Those predictions are not
reported as physical Xbox measurements.

## Inspect a local Wine Explorer without executing it

```sh
python3 desktop/wine-explorer/audit_imports.py /path/to/explorer.exe --output report.json
```

The audit uses GNU `objdump`, including named Wine x64 linker thunks. Some Wine
builds have a zero PE delay-directory entry while their actual code still calls
delayed `shell32`, `ole32`, `oleaut32`, `comctl32` and `shlwapi` imports. Inspecting
only the ordinary import table would miss the Explorer shell dependencies.
Stripped files and dynamic `GetProcAddress` calls require further inspection.
Literal matches in bridge resolver source are labeled **candidates**, not
resolved or working APIs; semantic and physical testing remain necessary.

The installed open-source Wine 10.0 x64 Explorer audit found 218 entries:
182 ordinary imports and 36 named delayed thunks across 13 DLLs. Notable groups
with no matching bridge resolver name include:

- loader/runtime: `DelayLoadFailureHook`, `ResolveDelayLoadedAPI`, Wine's
  `win32u!NtUserMessageCall`, `rpcrt4!UuidCreate`, `ntdll!NtSetInformationProcess`,
  and the binary's 34 `ucrtbase` imports;
- shell PIDLs: `ILClone`, `ILCloneFirst`, `ILCombine`, `ILCreateFromPathW`,
  `ILFindChild`, `ILFree`, `ILGetNext`, `ILGetSize`, `ILIsEqual`, `ILIsParent`;
- shell services: `SHBindToParent`, `SHGetIDListFromObject`, `SHGetImageList`,
  `SHGetKnownFolderPath`, `CoRegisterClassObject`, `StrRetToStrW`,
  `PathIsDirectoryW`, `PathRemoveBackslashW`, and OLE Automation arrays/variants.

That binary is Wine 10.0, not the pinned Mihawk Wine build. Counts describe
that audit sample and do not assert the complete imports of a future port.
