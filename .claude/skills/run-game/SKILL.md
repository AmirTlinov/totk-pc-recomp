---
name: run-game
description: Run a TotK build into gameplay from a backed-up save. Use when you need the game in-world (screenshot, perf measurement, frame dump, scripted input) or need to pick a save scene.
---

# Run a build into gameplay

One command loads a save and plays scripted input; it never writes the user's profile (`build/profile`):

```
.venv/Scripts/python tools/run_game.py <build dir> --save <backup> --frames N [--script tools/input/<f>.txt] [--shot out.png] [--env K=V] [--quiet] [--tag name]
```

It copies the profile, swaps in the save from `build/save-backup/`, writes the log to `<build dir>/logs/<tag>.log` and
prints `<log>: rc=<code>`. The game window opens behind the user's windows without taking focus.

## Saves

| `--save` | Scene after `load_save.txt` (gameplay from about frame 1500) |
|---|---|
| `user-save-2026-10-21-field` | Hyrule field, Link in tall grass |
| `user-save-2026-10-21-monsters` | Moblins and Zonai constructs attack at once; use with `--script tools/input/fight.txt` |
| `user-save-2026-10-21` | Hyrule field facing the castle, rainy; distant sky over the castle |
| `user-save-2026-10-23-pillars` | Hyrule field by standing stone pillars, about 17:40, a Zonai construct beside Link; use with `--script tools/input/backrun.txt` (runs backward from 1550: phantom rocks, a one-frame glitch at about 1605) |
| `user-save-2026-10-24-brightbloom` | Cave, about 20:40: a Brightbloom plant at the lower left (its bulb floats beside the stem, `build/v121-bloom/NOTES.md`), teal Moblin and Bokoblin ahead or Zonai constructs (runs differ) |
| `user-save-2026-10-22-island` | Sky island edge; use with `--script tools/input/island.txt` (runs off, skydives, opens the paraglider with B at frame 1975; X does nothing in a dive) |

`--frames` counts presents. Pick `1500 + the frames you need in-world`.

At most 3 runs go at once across all agents (`TOTK_MAX_RUNS`, lock files in `build/run-slots/`): more exhaust device
memory. `run_game.py` waits for a free slot, so start every game run through it; probes (`--probe-*`) run directly.

## Options that change results

- `--shot` builds pipelines synchronously: use it for pictures, never for timing.
- `--quiet` waits until no other `totk.exe` or `ninja.exe` runs. Use it for every timing you report.
- Input scripts: lines `<frame> <button> [hold]` or `<frame> LSTICK|RSTICK x y [hold]`, names in `docs/controls.md`.
  A new script starts with the four A presses of `tools/input/load_save.txt`. The Purah Pad map opens with
  `<frame> MINUS 4`.

## Recordings of the user's play

`TOTK_INPUT_RECORD=<absolute path>` writes the merged input as script lines; `TOTK_SHOTS=lo-hi:step` with
`TOTK_SHOTS_DIR=<absolute path>` saves 480x270 frames. The user's sessions live in `build/recordings/` (`<name>.txt` +
`<name>-shots/`). Replay one with `--script build/recordings/<name>.txt`; runs can diverge (weather, positions), so check
the shots of your replay before relying on it.

## Flight recorder

`TOTK_FLIGHT=1` keeps the last 600 frames (`TOTK_FLIGHT_FRAMES`): interval, render-thread blocked time, event counts
and a 320x180 thumbnail each. F8 in the game window, a frame over `TOTK_FLIGHT_SPIKE_MS` (100; one dump per 10 s) or
`TOTK_FLIGHT_AT=<frame>,...` writes `build/flight/<time>-f<frame>/` (`frames.tsv`, `f<frame>.png`) 30 frames later;
the log line `flight: dump <dir>` names it. `python tools/flight_view.py <dir>` lists the frames around the worst
interval and the counts that stand out, and writes `contact.png`. Costs 132 MB of host memory for the thumbnails.

## Running totk.exe yourself

Set `TOTK_ASSUME_FOCUS=1` (an unfocused window stalls the game), `TOTK_INPUT=none` and `TOTK_BACKGROUND_WINDOW=1`.
Other agents run the game at the same time: stop only a `totk.exe` you started, by PID.

Measuring, frame dumps and debugging: `docs/agent-playbook.md`.
