# Patches against pinned Wine

Base: `ValveSoftware/wine` `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`.

| Patch | Purpose |
| --- | --- |
| `0001-xcdm-guest-ntcurrentteb-redirect.patch` | With `-DXCDM_GUEST`, PE modules call `ntdll!NtCurrentTeb` instead of reading `gs:[0x30]`; adds the x86_64 export. See `docs/WINE_GUEST_ENVIRONMENT.md`. |

Apply from the Wine source root:

```sh
git apply /path/to/xbox-consoles-desktop-mode/patches/wine/0001-xcdm-guest-ntcurrentteb-redirect.patch
```

These patches modify Wine sources and are distributed under Wine's
LGPL-2.1-or-later. A distributed build that includes them must provide the
corresponding source, as for any other Wine change.
