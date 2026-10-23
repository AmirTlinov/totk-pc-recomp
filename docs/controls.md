# Controls

Input comes from the first SDL3 gamepad and the keyboard, merged into one Switch controller (`runtime/input_win.c`). Buttons are ORed; each stick takes the source with the larger deflection. Keys count only while the game window is in the foreground. `TOTK_INPUT=none` disables both (automated runs). `TOTK_*` settings can also be set in `totk.ini` next to `totk.exe` (`input = none`, `motion = 0`, `rumble = 0`); the environment overrides the file.

| Switch | Gamepad (by position) | Keyboard |
|---|---|---|
| Left stick | left stick | W A S D |
| Right stick | right stick | I J K L |
| D-pad | D-pad | Arrow keys |
| A | east | E |
| B | south | Left Shift |
| X | north | Space |
| Y | west | F |
| L / R | shoulders | Tab / R |
| ZL / ZR | triggers (over 50%) | Q / Z |
| Left / right stick click | stick clicks | C / V |
| Plus / Minus | Start / Back | Enter / Backspace |

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. A gamepad plugged in later is picked up within one second; with none connected the log says `input: no gamepad`.

## Scripted input

`TOTK_INPUT_SCRIPT=<file>` replays presses by presented-frame number (`gfx_counters.frames`). It is ORed with real input and still applies under `TOTK_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
```

Each event logs once (`input: script frame N: A`). `tools/input/load_save.txt` loads the first save from the title screen.

## RenderDoc capture (blocked)

`TOTK_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `TOTK_RDOC_DIR`, `TOTK_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which the back end requires for guest pool imports (`runtime/vk_memory.c`), so device creation fails under it. A capture needs a copy-based pool fallback first.
