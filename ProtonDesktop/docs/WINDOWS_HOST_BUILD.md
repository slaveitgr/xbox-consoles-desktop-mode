# Future Windows host compile check

Not run yet. This compiles the native UWP host starter, which reports that the
Boxedwine64 backend is unavailable. It does not compile the emulator, start Wine
or produce Xbox compatibility evidence.

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

Expected executable: `build/uwp/Release/ProtonDesktop.exe`. Archive the build log
and exact compiler/SDK versions if this is later run in Windows CI. Do not sign,
install or deploy the unfinished host as part of a compile check.

The CoreApplication bootstrap uses an MTA apartment and catches top-level
HRESULT failures. Device removal/reset still reports failure; graphics-device
recreation and suspend/resume are unfinished. A successful build therefore
qualifies compilation of this host only. The actual Windows guest-x64 build,
UWP API port and Wine/Explorer runtime gates remain separate and false.
