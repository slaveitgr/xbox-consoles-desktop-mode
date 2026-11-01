# Physical Xbox testing

Target first: retail Xbox One / One S / One X in Developer Mode. Record the
exact model and OS version for each run. The first Xbox One X baseline is
recorded in `VALIDATION.md`; other models have not been tested by us.

## Companion baseline

1. Build/sign the diagnostics package as described in `BUILD.md`.
2. Install it through Xbox Device Portal with the matching dependencies and
   signing requirements for your development setup.
3. Launch it outside the Visual Studio debugger so debugger-only resource
   behavior does not contaminate the baseline.
4. Both probes run automatically on launch. Record the executable-memory
   probe's actual status/error code; the buttons also allow individual reruns.
5. Record the minimal-PE probe's actual status/error code.
6. Both-test runs save the report automatically. Retrieve `runtime-diagnostics.txt`
   from the app's LocalState using Device Portal,
   and retain its console model/OS details next to the results.
7. Check controller reconnect, keyboard input, resize/resume and repeated probe
   runs. Repeat failures should retain the same error details rather than
   becoming a silent pass.

## Controller detection

The 0.2 companion adds a Controllers view for devices exposed to the application
by `Windows.Gaming.Input`. This is an input diagnostic, not a USB/Bluetooth
driver or a change to the console's accessory compatibility.

1. Start with one known Xbox-compatible controller, then test each additional
   controller separately. Record its exact model and connection: wired USB,
   receiver/dongle, or Xbox Wireless. A USB cable may provide charging only.
2. Record both Gamepad and RawGameController enumeration results. They are
   overlapping API views, so their counts must not be added together.
3. Select the device and check buttons, sticks, triggers and available raw
   switches. Record actual readings, not merely an enumerated device name.
4. Save the report after connecting or disconnecting a device. No exposed device
   is distinct from an API failure; retain the actual error if one occurs.
5. Unplug/reconnect and suspend/resume, then verify detection and release of
   previously held inputs. Keep these physical tests separate from compilation.

Xbox 360/PC gamepad support in a Windows API does not establish direct Xbox One
console compatibility. Microsoft's Xbox UWP limitations list generic HID and
Bluetooth APIs as unsupported; adding those manifest capabilities does not
provide a controller driver. See
[Xbox UWP limitations](https://github.com/MicrosoftDocs/winrt-related/blob/docs/winrt-related-src/extension-sdks/uwp-limitations-on-xbox.md)
and [Gamepad API](https://learn.microsoft.com/en-us/windows/uwp/gaming/gamepad-and-vibration).

## What a pass means

The memory test proves one small x64 function ran after an RW-to-RX transition.
The PE test proves only our self-authored minimal image ran under our explicit
entry contract. Neither test exercises normal EXE initialization, imports,
thread-local storage, graphics, .NET, Steam services, DRM or anti-cheat.

## Upstream desktop baseline

Once the full upstream desktop has been built with its external Gecko inputs,
record shell launch, file access, mouse/keyboard behavior and one exact guest
application/version/hash. Keep upstream developer reports distinct from these
local results. Native USB mouse behavior also needs verification under any
changed downstream package identity.

Attach logs to a local report or issue deliberately; do not include login tokens,
account names, private paths or game files in public reports.
