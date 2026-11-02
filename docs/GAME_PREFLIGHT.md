# Direct game-folder inspection

The native UWP companion opens on **Game preflight**. This is the first host
integration for the user's classic PC San Andreas files. It reads the selected
executable and directory metadata; it does not invoke guest code.

## Use

1. Keep the installed game's directory structure on the USB drive, including
   `gta_sa.exe` and its existing subfolders and DLLs.
2. Launch Xbox Desktop Diagnostics and select **Choose game folder**.
3. In the system picker, select the folder that directly contains `gta_sa.exe`.
4. The report is saved automatically to the companion's
   `LocalState/game-preflight.txt`, accessible through Device Portal.
5. **Rescan selected folder** uses the current folder or its saved
   FutureAccessList grant. Choose the folder again if the USB was removed or
   the saved grant is unavailable.

Keyboard: **O** chooses, **R/F5** rescans and **S** saves the current report.
Arrows/Tab select an action and Enter activates it. Gamepad A activates and X
saves. **Y** on a gamepad or **C** on a keyboard cycles Game, Runtime and
Controllers. Mouse interaction is also available.

The app does not need a broadFileSystemAccess declaration for a picker-granted
folder. Microsoft lists folder-picker scenarios as unsupported on Xbox, so
physical testing is necessary even when an installed third-party shell can
browse the USB. Unsupported/cancelled picks, missing media, missing EXEs and
read failures must produce an explicit status instead of a compatibility claim.

## Report scope

- A separate portable parser accepts bounded PE32 and PE32+ metadata. It never
  calls an entry point or changes executable memory protections.
- Reads are limited to 64 MiB per executable. Root-directory listings are
  limited to 256 files; the report identifies a listing that reaches this cap.
- Architecture, headers and imports identify the actual executable's initial
  requirements. Graphics indicators come from static imports and cannot prove
  which APIs are selected at runtime or which libraries are loaded dynamically.
- Sibling filenames and the presence of `data`, `models`, `audio`, `text`,
  `scripts` and `plugins` help identify the installation and possible mods.
- Only the text report is saved by the host. The game remains in its selected
  location. Game data and executable files must not be committed to this repo.

Classic San Andreas is documented by the prospero-win profiles project as
**PE32 / x86 and Direct3D 9**. The existing DesktopMode bootstrap's PE32+
restriction is therefore an expected incompatibility. Our current runtime also
has no Windows x86 execution or D3D9 game implementation. Removing a machine
check does not provide either component. A successful inspection is a storage
and metadata milestone, not a game-launch milestone.

## Validation

Record Linux parser tests, Windows native tests, UWP packaging and physical
Xbox inspection separately. Console inspection must name the exact app version,
whether USB selection worked, and the retrieved report. Do not call a host
test or package build an Xbox result.

## Sources

- [Microsoft: picker-granted file access](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions)
- [Microsoft: Xbox UWP limitations](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox)
- [Microsoft: PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
- [Prospero San Andreas profile](https://github.com/mpereiraesaa/prospero-win-profiles/blob/2db551b59ecfc3302c79d431224b74dcda16d880/profiles/gta-san-andreas.profile)
