# Architecture

## Desktop foundation

`external/DesktopMode_uwp` is the existing C# UWP shell plus its C++/CX
Win32Bridge. The shell's internal windows run in its UWP process. It does not
replace the Xbox operating system.

The bridge parses x64 PE32+ images, maps them into memory, applies relocations,
binds selected imports and invokes their entry points. Its Kernel32, User32,
GDI32 and related compatibility code supplies a virtual filesystem/registry
and classic GUI behavior. Guest software drawing goes through MiniGdi buffers
and XAML `WriteableBitmap`; this is not an implemented Direct3D gaming backend.

## Diagnostics companion

`app/DiagnosticsApp.cpp` is an independent native C++/WinRT UWP app. It exists to
test the platform without the upstream desktop's Gecko/Calculator dependencies.
It does not replace that desktop or duplicate its window manager.

The shared `runtime/` code contains a bounded, constrained PE parser and two
explicit tests: writable-memory to executable-memory transition, and a mapped
minimal self-authored PE. Memory is never writable and executable at once.
The loader is intentionally narrower than the upstream bridge: it rejects
normal application dependencies rather than pretending to implement them.

`samples/` defines the tiny return-42 program; `tests/` validates parser and
platform behavior on a host. CMake builds these host checks, while the Visual
Studio project builds the UWP companion.

## Future Steam work

Library access, authenticated downloads and game execution are separate
interfaces. Playden may provide useful code for the first two after adapting
transport/storage and resolving licensing. Neither a library UI nor a successful
return-42 probe proves that Steam's desktop client or a game will work.

Before a graphics integration, select one owned, offline test title, enumerate
its real imports and graphics requirements, then measure against those exact
requirements. No game or account data belongs in the source tree.
