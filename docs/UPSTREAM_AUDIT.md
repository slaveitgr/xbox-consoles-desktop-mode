# Upstream audit

Inspected 2026-11-02 at DesktopMode UWP commit
`f7df20d3abac53f6996acef4fe93d6482a3542b4`.

## Confirmed by source inspection

- The desktop implementation already exists as a C# UWP app. Reusing it avoids
  recreating its Start menu, taskbar, file tools and internal-window behavior.
- `Win32Bridge/Bridge/PeImage.cpp` and `PeMapper.cpp` target AMD64 PE32+ images.
- `GuestRuntime.cpp` prepares the image and actually calls the guest entry point.
- `RuntimeImage.cpp` uses writable allocation followed by executable protection.
- The bridge includes substantial but selective Win32 API implementations.
- `GuestWindow.cpp` copies software-rendered BGRA surfaces to XAML
  `WriteableBitmap`; no guest D3D/DXGI backend was found in the shim resolver.
- The C# host uses the C++/CX `RuntimeSession` component with `PrepareAsync`,
  `RunAsync`, input control and close methods.

## Reported by upstream, not verified on our Xbox

`Win32Bridge/BRIDGE.md` reports earlier Win7 Notepad graphical rendering,
near-complete 7-Zip rendering/file extraction and partial Notepad++ UI. These
reports do not establish complete application support or Steam/game support.

## Build constraints

- The full solution references a Gecko project requiring separate patched
  Gecko source, a compiled object directory and its runtime DLLs. An ordinary
  recursive clone does not provide those inputs.
- Current native projects use the v143 toolset and Windows SDK 10.0.26100.0.
- Original package identity/certificate assumptions and bundled content need a
  separate downstream packaging pass. Our companion uses its own identity and
  generated artwork and does not publish a rebranded upstream package.
- Building only `Win32Bridge.vcxproj` checks that native component; it is not a
  complete desktop package build or console compatibility test.

## Sources

- [DesktopMode overview](https://github.com/Pedro1234-code/DesktopMode_uwp)
- [Bridge status](https://github.com/Pedro1234-code/DesktopMode_uwp/blob/f7df20d3abac53f6996acef4fe93d6482a3542b4/Win32Bridge/BRIDGE.md)
- [Microsoft executable-memory API](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp)
- [Microsoft Xbox UWP resources](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
