# Installed desktop baseline

The published [DesktopMode UWP release 21286](https://github.com/Pedro1234-code/DesktopMode_uwp/releases/tag/21286)
was deployed to the Xbox One X on **2026-11-02, Europe/Athens**. This gives the
project an installed desktop foundation while the independent source build is
being made reproducible.

## What is installed

- Main file: `DesktopMode_1.0.46.0_x64.msix`.
- Package identity: `Microsoft.MicrosoftEdge.CoreShell`.
- Version: `1.0.46.0`, architecture: `x64`.
- Launch identifier: `Microsoft.MicrosoftEdge.CoreShell_8wekyb3d8bbwe!CoreShellApp`.
- Main file SHA-256: `4691181e07f68f8d8c58e96ad44d4a01a9d2ccf1381669fd3ffa61d1e492beea`.

The main manifest requires these five frameworks. The matching x64 files are
available from the same upstream release and were supplied during deployment:

| Release file | Framework identity | Minimum version |
| --- | --- | --- |
| `Microsoft.UI.Xaml.2.8.appx` | `Microsoft.UI.Xaml.2.8` | `8.2501.31001.0` |
| `Microsoft.NET.Native.Framework.2.2.appx` | `Microsoft.NET.Native.Framework.2.2` | `2.2.29512.0` |
| `Microsoft.NET.Native.Runtime.2.2.appx` | `Microsoft.NET.Native.Runtime.2.2` | `2.2.28604.0` |
| `Microsoft.VCLibs.x64.14.00.appx` | `Microsoft.VCLibs.140.00` | `14.0.33519.0` |
| `Microsoft.VCLibs.x64.14.00.Desktop.appx` | `Microsoft.VCLibs.140.00.UWPDesktop` | `14.0.33728.0` |

This is the unmodified upstream release, not a package produced by our CI.
Downloaded binaries, local device certificates and private network details
are not committed to this repository. Our diagnostics companion remains a
separate application.

## Observed on Xbox One X

Device Portal returned deployment success (code 0) and accepted launch (HTTP
200). Process enumeration reported `DesktopMode.exe` running. The user confirmed
the desktop and taskbar on the TV. Screenshot capture through Device Portal
failed after launch; no desktop screenshot is claimed as evidence.

No CoreShell package was installed before deployment. The existing Edge Stable
identity is different, so this installation did not replace that package. The
desktop runs inside a UWP application; the Xbox system shell remains separate.

## Controls and next checks

Upstream recommends a USB mouse and keyboard. Its source maps the Windows key
to Start and Alt+Tab to Task View. These shortcuts have not yet been tested in
this session. Normal desktop launch does not require the optional loopback
exemption described upstream for accessing Device Portal from inside Strawfox.

Next, check Start, window movement, Files, Notepad, Calculator, keyboard/mouse
input and suspend/resume on the console. Test the upstream executable bridge
separately from our minimal diagnostic probes. Steam authentication, library
downloads and PC game graphics are still separate development work.

The full shell source build needs the patched external Gecko source and runtime
described in [`BUILD.md`](BUILD.md). Installing this release does not resolve
that source-build prerequisite.
