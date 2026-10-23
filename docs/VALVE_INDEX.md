# Valve Index support

Use SteamVR as the active OpenXR runtime and turn on both controllers before launching.
The mod automatically detects the Valve Index interaction profile per hand. Existing
Touch, Vive, WMR and simple-controller bindings remain available.

| Control | Action |
| --- | --- |
| Left stick | Move; click to sprint |
| Right stick | Turn; up/down zooms with the camcorder raised |
| Right stick click | Night vision |
| Left A | Interact / open |
| Left B | Reload battery |
| Right A | Jump / vault |
| Right B | Crouch / drop |
| Left / right trigger | Lean; look back while running |
| Close / open hand | Grab / release doors, climbing holds, camcorder (hands-on mode) |
| Left grip | Sprint outside an interaction |
| Left trackpad pressure press / release | Pause |
| Left grip + left trackpad | VR settings |
| Both stick clicks | Recenter |
| Left grip + left A / left B / right B | Recordings / documents / notebook |

System buttons retain their SteamVR dashboard function. Haptics use the existing game
rumble and interaction feedback settings. In the launcher's Controls page, click
**I: SHOW INDEX** or press **I** to view this layout.
The trackpad uses pressure thresholds of 0.55 to press and 0.35 to release, so simply
resting a thumb on it does not open a menu. Lost focus or a failed input sync clears
the published controller state to avoid stuck grabs and buttons.

## Natural grab settings

`[Input] IndexGrabClose=0.80` and `IndexGrabOpen=0.55` use the controller's
`squeeze/value` hand-closure signal, not `squeeze/force`. Between these thresholds
the previous grab state is retained, preventing accidental repeated grabs. Adjust
the values if needed; require `0 <= Open < Close <= 1`. Restart after changes.
These settings apply only to detected Index controllers. Keep the existing hands-on
camcorder mode for grab/release; the optional grip-button mode still toggles it.

## Finger animation

`[Input] IndexFingerTracking=1` enables optional `XR_EXT_hand_tracking` at startup.
When the runtime exposes valid joint data for an Index hand, the mod derives an
independent curl for each finger and applies it to Miles's existing finger chains.
This is curl animation, not full skeletal hand retargeting or finger splay.
Invalid/inactive data, missing extension functions, or tracker creation failure
fall back to native finger poses. Door/camcorder grips and authored animations
retain priority. Set this option to `0` and restart to disable it.

The log reports the active profile per hand, extension availability, tracker
creation results, and the first valid finger sample per hand. Extension availability
does not guarantee the runtime supplies controller-derived fingers.

## Validation

Run `scripts\test_index.cmd`, `scripts\build.bat`, and `launcher\build_launcher.cmd`.
The automated checks cover grab hysteresis/release, finger angle invariance,
OpenXR x64 structure sizes, and Index/Touch/mixed-controller hint rewriting.

Hardware verification remains required: test menu/dashboard focus transitions,
controller disconnection/reconnection, relaxed vs closed hands, door release,
climbing, camcorder pickup, every button, vibration, finger movement and missing
finger tracking. No physical Index validation was performed during implementation.

API reference: [Khronos OpenXR specification](https://registry.khronos.org/OpenXR/specs/1.1-khr/html/xrspec.html),
[official header](https://github.com/KhronosGroup/OpenXR-SDK/blob/main/include/openxr/openxr.h).
