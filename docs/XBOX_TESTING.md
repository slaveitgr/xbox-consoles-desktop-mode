# Physical Xbox testing

Target first: retail Xbox One / One S / One X in Developer Mode. Record the
exact model and OS version for each run. No console result has been recorded
in this repository yet.

## Companion baseline

1. Build/sign the diagnostics package as described in `BUILD.md`.
2. Install it through Xbox Device Portal with the matching dependencies and
   signing requirements for your development setup.
3. Launch it outside the Visual Studio debugger so debugger-only resource
   behavior does not contaminate the baseline.
4. Run the executable-memory probe and record its actual status/error code.
5. Run the minimal-PE probe and record its actual status/error code.
6. Save the report, retrieve it from the app's LocalState using Device Portal,
   and retain its console model/OS details next to the results.
7. Check controller reconnect, keyboard input, resize/resume and repeated probe
   runs. Repeat failures should retain the same error details rather than
   becoming a silent pass.

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
