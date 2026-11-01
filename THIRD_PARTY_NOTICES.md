# Third-party sources

## DesktopMode UWP

- Repository: https://github.com/Pedro1234-code/DesktopMode_uwp
- Pinned commit: `f7df20d3abac53f6996acef4fe93d6482a3542b4`
- Location: `external/DesktopMode_uwp` (Git submodule, original source retained)
- License: upstream MIT, with component-specific notices inside the submodule.
- Upstream credits the original factoryos-10x-shell, Wine, ReactOS, Windows10x,
  Files and Windows Calculator. Preserve its notices when deriving code.

The diagnostics companion does not copy upstream icons, audio, certificates,
guest applications, Gecko binaries, Wine/Proton libraries or commercial games.
Building the upstream full desktop is a separate operation with its own inputs.

## Research references (not bundled)

- Playden: https://github.com/leolobato/playden-ps5 (GPL-3.0; future integration
  requires honoring the applicable license rather than treating code as MIT).
- MihawkOS: https://github.com/mihawk-99/PS5_MihawkOS (desktop/runtime design reference).
- Xbox Win32 Runner: https://github.com/http-hadi/xbox-win32-runner (research
  reference; no source copied into our probes).

The minimal return-42 PE fixture and original package artwork are authored for
this repository. No third-party Windows program is required for those tests.
