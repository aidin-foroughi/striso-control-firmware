# Striso Control Firmware: expressive synth fork

This is an **unofficial fork** of the [Striso board](https://www.striso.org) firmware ([striso/striso-control-firmware](https://github.com/striso/striso-control-firmware)). It turns the board's internal synth into a multi-voice expressive instrument, makes the Striso a USB sound card, and comes with a desktop app, [Striso Live](https://github.com/aidin-foroughi/striso-live).

Everything the official firmware does still works: MPE/MIDI, tunings, presets, the config editor and the UF2 bootloader. For the official firmware and support, see [striso.com](https://www.striso.com) and the [Striso forum](https://forum.striso.com).

## What this fork adds

- **Seven voice engines on the board, with presets:**

  | Settings key | Voice | Presets |
  |---|---|---|
  | grey | **strisy** | the original sound, rewritten in C++ (about 5× less CPU) |
  | orange | **VA** (virtual analog) | classic, pad, lead, bass, brass, keys, acid, strings |
  | blue | **modal** (bars and bells) | vibraphone, marimba, glockenspiel, bell, kalimba, wine glass |
  | red | **bowed** (strings) | violin, viola, cello, bass, section, ponticello, flautando, sordino, pizzicato |
  | green | **wind** (reeds and flute) | clarinet, bass clarinet, soprano/alto/tenor/baritone sax, oboe, bassoon, flute |
  | purple | **EP** (electric pianos) | stage, suitcase, mk2, dyno, ballad, bark, neosoul, fm80s, lofi, wurli, wurlibark, pianet, felt, celesta |
  | amber | **brass** | trumpet, flugelhorn, trombone, horn, euphonium, tuba, harmon, plunger, section |

  Every voice responds to pressure, strike velocity, tilt (x/y) and the pedal. The bowed voice, for example, maps pressure to the bow, tilt to bow position and a strike to a bite.
- **10-voice polyphony and stereo master effects:** chorus, delay, reverb and tremolo.
- **USB audio:** the Striso is a class-compliant USB sound card, **"Striso Synth"** (48 kHz stereo, 24 or 16 bit, no driver). You can record the synth in any DAW or hear it through your computer. The headphone jack keeps playing at the same time. The audio clock now runs at exactly 48 kHz.
- **Live control over USB:** every synth and effect parameter can be read and set while you play. So can the global touch sensitivity (pressure, velocity, tilt, bend, threshold) and a per-key sensitivity factor. You can save them to the board.
- **Fixes:**
  - The motion sensor's I²C bus recovers after a reboot.
  - The key stream saturates instead of wrapping on hard presses.
  - The system reset (settings + bottom-right key) waits until the settings button is released, so it no longer drops into the bootloader by accident.

## Choosing voices and presets on the board

1. Hold the **settings** (square) button.
2. Press a key in the row the official settings card marks **"(future use)"**: one key per voice, and the indicator LED shows the voice's colour.
3. To step through that voice's presets, keep the key pressed and **nudge it up or down**, or **circle** it. The up/down LEDs show the preset number. Each voice remembers its last preset until power off.

[`doc/quick_start_guide.pdf`](doc/quick_start_guide.pdf) has the full settings card. [Striso Live](https://github.com/aidin-foroughi/striso-live) also shows it as an overlay on the board.

## Striso Live

[Striso Live](https://github.com/aidin-foroughi/striso-live) is a free desktop app for Windows and macOS. With it you can:

- play with every sound parameter;
- save your own sounds;
- see scales, chords and roman numerals as you play;
- tune touch sensitivity and even out individual keys, then save the result to the board;
- hear the Striso through your computer and record it.

Install on macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/aidin-foroughi/striso-live/main/install-mac.sh | bash
```

Install on Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/aidin-foroughi/striso-live/main/install-windows.ps1 | iex
```

## Updating firmware

1. Plug in the Striso board with the **settings (square) button** held down. It attaches as a flash drive.
2. Copy a `.uf2` file to that drive. The board reboots by itself.
3. The current version is in `INFO_FW.TXT`. Copy `CURRENT.UF2` first to keep a backup.

A firmware update doesn't touch the bootloader or your saved settings. To go back, copy an [official release](https://github.com/striso/striso-control-firmware/releases/latest) or your backup the same way.

The UF2 bootloader is at https://github.com/striso/uf2-ChibiOS.

## Building

The build runs in Docker (Ubuntu 22.04, arm-gcc 10.3, Faust 2.37.3). Built from the official v2.2.4 source, this toolchain reproduces the official release byte for byte. To build:

```sh
git submodule update --init
tools/build.sh uf2 VOICE=all      # all voices; VOICE=strisy (default) or VOICE=<name> builds one
```

After the build, `tools/build.sh` checks the firmware. If any check fails, it deletes the `.uf2` so it can't be flashed. It checks that:

- no soft-float helper code got in (the DSP must use the FPU);
- the synth thread's stack is large enough (a call-graph check);
- the load address is right.

On Windows, clone with `git config core.autocrlf false`. Otherwise CRLF line endings end up in the generated config editor.

Requirements without Docker: gcc-arm-none-eabi, faust (only for the Faust engine), git (for the version number) and python3 (for the UF2 file).

## Tools

`tools/` holds the PC-side tooling. Install the Python dependencies with `python -m venv tools/.venv && tools/.venv/Scripts/pip install -r tools/requirements.txt` (on macOS/Linux: `tools/.venv/bin/pip`).

- `tools/striso.py`: talks to the board over USB:
  - read the version, CPU load and profile;
  - list and set live parameters (`params`, `set`) and select voices (`voice`);
  - save touch settings (`save-touch`);
  - flash safely (`flash`). It backs up first, refuses blocks outside the app region and health-checks the board after the reboot.
- `tools/harness/`: renders the voice engines on the PC from scripted or recorded performances (`render_strisy.sh`), with stress and stale-input tests. `tools/harness/rec/` has real key-stream recordings for replay.
- `tools/record_keys.py`: records the key stream from the board to CSV.
- `tools/review_voice.sh`: builds and checks a voice engine before it may be flashed.
- `tools/usbaudio/`: USB audio descriptor checks, a simulation of the audio buffer and clock-rate matching (servo), and a test-tone analyzer.

## Credits and licence

The original firmware, hardware and documentation are by Piers Titus van der Torren and contributors ([striso.com](https://www.striso.com)). This fork's additions are by Aidin Foroughi. The firmware is licensed under the GNU GPL v3 or later; see the source file headers.
