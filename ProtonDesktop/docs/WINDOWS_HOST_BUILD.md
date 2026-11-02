# Windows UWP host compile check

Passed on 2026-11-02 in
[Windows CI 41469175033](https://github.com/slaveitgr/xbox-consoles-desktop-mode/actions/runs/41469175033),
source `217a2791bfed70d0e224eb5c233816a402c8c3c6`.
The native x64 UWP executable linked with `/APPCONTAINER`; its SHA256 is
`edf30a0db4b02d6303f7d85e335b4cfac704de4e109df0c3ffe818caf07b0023`.
This proves host compilation. The backend is unavailable and no UWP session,
Wine process or Xbox test ran.

Required on Windows:

- Visual Studio 2022 / Build Tools with v143 C++ and C++ Universal Windows
  Platform development support.
- Windows SDK **10.0.26100.0**, including
  `Include/10.0.26100.0/cppwinrt/winrt/base.h` and x64 Universal libraries.
- MSBuild from that Visual Studio installation.

No Win32Bridge, WebView2, Wine rootfs or NuGet package is needed for this host.
From a Visual Studio developer PowerShell in the ProtonDesktop project root:

```powershell
msbuild ProtonDesktop.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:AppxPackage=false /p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false
```

Output executable: `build/uwp/Release/ProtonDesktop.exe`. The compile artifact
records the SDK, source revision and executable digest. The downloaded CI ZIP
was verified against SHA256
`5cf09f87c0b088a6240e5c0e97994455f762e77b5a1d7877203cc6ac8a7bf9df`.
No package is signed, installed or deployed by this compile check.

The CoreApplication bootstrap uses an MTA apartment and catches top-level
HRESULT failures. Resize/DPI, device recreation and suspend/resume handling
are implemented and compile, but those lifecycle paths have not been exercised
on a device. The actual guest-x64 core, UWP API port and Wine/Explorer execution
remain separate qualification steps.
