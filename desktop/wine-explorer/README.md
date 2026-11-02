# Real Wine Explorer integration

The requested target is Wine Explorer itself. The existing HTML Files app is
not Wine Explorer. This directory contains pinned upstream source staging and
a self-authored qualification guest; it does **not** yet provide a working
Wine Explorer on Xbox.

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

`source-lock.json` pins 63 files (1,940,102 bytes) by repository, commit, size and
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
`explorer.exe` or compiling `explorer.c` alone does not supply them. A full Wine
runtime inside an emulator is a separate route that may reuse more upstream
code, with its own performance, filesystem and packaging qualification.

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
