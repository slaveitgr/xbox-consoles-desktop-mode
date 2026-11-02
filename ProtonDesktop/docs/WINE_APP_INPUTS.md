# Native Wine application inputs

The separate `ProtonDesktop.Wine.vcxproj` produces package version `0.2.0.0`
under the existing `ProtonDesktop.Native` identity. Its native SDL/WinRT entry
calls the real Wine session. Building the package does not establish that Wine
or Explorer runs successfully.

The two private rootfs candidates are supplied separately in this app's
`ApplicationData::Current.LocalFolder` (`LocalState`):

```text
LocalState/
  Runtimes/
    glibc-rootfs64.zip
    wine64.zip
  WinePrefix/                   created/preserved by the application
  protondesktop-wine-session.log
  protondesktop-wine-result.json
```

The app verifies the ZIPs **before** creating/checking `WinePrefix`; it does not
create `Runtimes` or download missing inputs. The authorized device-staging
step must first create `LocalState/Runtimes` and upload both files there. If
using a Device Portal file-upload endpoint that does not create parent folders,
create the folder first, or upload a temporary outer archive containing
`Runtimes/glibc-rootfs64.zip` and `Runtimes/wine64.zip` into `LocalState` with
that endpoint's `extract=true` option. Extract only the outer transport archive;
the two mounted runtime ZIPs remain ZIP files. Do not stage them into the
installed package directory or assume that a root-folder upload creates their
parent directory.

The app accepts only these exact current candidates:

| File | Bytes | SHA-256 |
|---|---:|---|
| `glibc-rootfs64.zip` | 2,850,458 | `2507d717e4f860789ea54fa0d73a8db8b152827f74ee24d2665c81757b439822` |
| `wine64.zip` | 276,707,863 | `a2614a5fbda1e4783f30b2e0fb3a66759c2649a1d86250a386191ba43a8cc0ea` |

Verification uses app-aware wide-character file opens and SHA-256. Read-only
file leases remain open for the session, preventing replacement/writes after
verification. A missing or mismatched input stops startup and records the
failure. Existing prefix data is preserved; no prepared-prefix ZIP is required.
These candidate bytes remain unqualified for redistribution and are excluded
from the application package.

`WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine` is intentionally the Wine module
base: Wine selects its `x86_64-unix` and `x86_64-windows` children. A separate
guest `LD_LIBRARY_PATH` explicitly includes the private Unix directory followed
by `/lib/x86_64-linux-gnu` and `/usr/lib/x86_64-linux-gnu` for ELF dependencies.
The metadata audit found all 29 previously pending dependency records in that
private directory: they refer to `ntdll.so` or `win32u.so`, both present with
matching SONAMEs. This supplies static search candidates; it does not prove
runtime loading. The ZIPs also contain the ELF interpreter, Wine NLS data,
Explorer/shell DLLs, fontconfig configuration and fonts. No fontconfig-path
override is needed for their installed layout.

`WINESYSTEMDLLPATH=\??\Z:\usr\x86_64-w64-mingw32\lib` supplies Wine 8's
additional **NT-format PE DLL search directory**. This is separate from the
ELF `LD_LIBRARY_PATH` above. The fixed ZIP already contains the genuine x64
`zlib1.dll` there; without this route, user32's zlib import fails and Wine's
delay-load failure hook misleadingly names `CreateDialogParamW`. A real Linux
guest trace confirmed that this environment-only change loads zlib, loads
user32 and resolves that function successfully, without changing either ZIP.
This is not proof of a completed prefix or a desktop on Xbox.

The app performs Wine version validation, runs Wine's non-resetting `--init`
path on each start, then requests Explorer within the same guest kernel.
Existing hive files can be left by an interrupted boot and do not bypass that
initialization. This does not force a `wine.inf` update or guarantee repair of
an interrupted INF update. Neither a loaded process nor a
successful build is evidence of desktop pixels or a stable running app. Read
the actual phase log/result and validate presentation separately.

The initial stock Wine command is:

```text
/usr/lib/wine/wine64 explorer.exe /desktop=shell,1920x1080 explorer.exe C:\
```

Wine 8's special desktop name `shell` enables its built-in taskbar/Start menu
without a prior registry edit. An arbitrary desktop name defaults that shell
off. The trailing explicit `explorer.exe C:\` opens the real file manager;
the desktop argument by itself opens no folder. This is Wine's own shell;
Windows 11-inspired theming remains a later step.

References: [Wine 8 desktop shell defaults](https://github.com/wine-mirror/wine/blob/wine-8.0/programs/explorer/desktop.c#L793-L814),
[Wine desktop child command and message loop](https://github.com/wine-mirror/wine/blob/wine-8.0/programs/explorer/desktop.c#L1051-L1078),
[Wine 8 loader module search](https://github.com/wine-mirror/wine/blob/wine-8.0/loader/main.c#L117),
[Wine module paths](https://github.com/wine-mirror/wine/blob/wine-8.0/dlls/ntdll/unix/loader.c#L509),
[glibc 2.36 ELF lookup](https://github.com/bminor/glibc/blob/glibc-2.36/elf/dl-load.c#L1814).
Local byte/provenance evidence is recorded in
`rootfs/.cache/assembly-evidence/assembly-report.json` and
`rootfs/.cache/assembly-evidence/payload-manifest.json`; those reports are not
packaged runtime inputs.
