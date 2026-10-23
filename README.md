# Striso Control Firmware: expressive synth fork

## Thank you, Piers

The [Striso](https://www.striso.com) is one of the most expressive instruments around. Every key senses pressure, strike velocity and tilt in two directions, and the keys sit in an isomorphic layout where every chord and scale has one shape in every key. Pressing a key feels like touching a string, not pushing a button. The Striso is designed and built by **Piers Titus van der Torren**, along with its hardware, firmware, bootloader, configuration editor and documentation, all of it thoughtful, careful work.

Piers released the firmware as **open source** under the GPL. That's the only reason this fork can exist: it builds directly on his firmware, his key sensing, his MIDI/MPE implementation and his bootloader. Thank you for making something this good and for sharing it. If you like what you hear here, the instrument deserves the credit. Go to [striso.com](https://www.striso.com), the [official firmware](https://github.com/striso/striso-control-firmware) and the [Striso forum](https://forum.striso.com).

This is an **unofficial fork**, not supported by Striso.

## About this fork

**It's vibe coded.** Practically all of the code in this fork was written by an AI coding assistant ([Claude Code](https://claude.com/claude-code)), directed by a musician playing the instrument and saying what sounded wrong. It was tested the hard way: on a real Striso board, with recorded real playing replayed through the engines, memory sanitizers and fuzzing on the PC, and a build that refuses to produce firmware if the DSP would fall off the FPU or overflow its stack. **It works really well.** It's still a fork made for fun, so keep a backup of your current firmware (copy `CURRENT.UF2`, see [Updating firmware](#updating-firmware)).

**The Striso is still a standalone instrument.** Plug in headphones or a speaker and play; you just have many more sounds to play with. Everything the official firmware does still works: MPE and normal MIDI, tunings, presets, the config editor and the UF2 bootloader. [Striso Live](https://github.com/aidin-foroughi/striso-live), the desktop app below, is an **optional companion**: nothing needs it.

## Big features

### Modelled instruments on the board

The internal synth now has seven voice engines, several of them physical models that react to the Striso's touch the way the real instruments react to a player:

- **Bowed strings:** a digital waveguide string driven by a friction bow with a modelled body. Pressure is the bow, tilt moves the bow from fingerboard to bridge, and a hard strike gives a short bite. Includes pizzicato.
- **Woodwinds:** physically modelled reeds and flute (clarinets, saxophones, oboe, bassoon). Pressure is breath.
- **Brass:** a lip-reed model with a physical bore (trumpet, flugelhorn, trombone, horn, euphonium, tuba, with harmon and plunger mutes).
- **Electric pianos:** physically inspired tines and reeds (Rhodes, Wurlitzer, Pianet and more): beautiful harmonics, a bit of grit and lots of presets.
- **Tuned percussion:** modal synthesis of real partials (vibraphone, marimba, glockenspiel, bell, kalimba, wine glass); pressure after the strike rubs and sustains.
- **Virtual analog:** saws, sync, a ladder filter and expressive envelopes.
- **strisy:** the original Striso sound, rewritten in C++ (about 5× less CPU).

All of them play with **10-voice polyphony** through **stereo master effects** (chorus, delay, reverb, tremolo).

### USB audio

The Striso is now also a **class-compliant USB sound card**, called **"Striso Synth"** (48 kHz stereo, 24 or 16 bit). It needs no driver on Windows or macOS. You can record the internal synth straight into any DAW or hear it through your computer, while the headphone jack keeps playing at the same time. The audio clock was retuned to run at exactly 48 kHz.

### Striso Live, the optional companion app

[**Striso Live**](https://github.com/aidin-foroughi/striso-live) is a free desktop app for Windows and macOS. Everything in it is optional, but it makes the new sounds easy to explore. With it you can:

- turn every sound and effect parameter live while you play;
- save your own sounds, and A/B them against the preset;
- see scales (66 of them, including bebop and Barry Harris), chord names and roman numerals on the board as you play;
- set touch sensitivity and even out individual keys, then save them to the board;
- hear the Striso through your computer and record it to WAV;
- play full screen in stage mode.

## What this fork adds, in detail

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

  Every voice responds to pressure, strike velocity, tilt (x/y) and the pedal.
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

## Installing Striso Live (optional)

[Releases](https://github.com/aidin-foroughi/striso-live/releases/latest) have a Windows installer, a Windows zip and a macOS `.dmg`. Or use one line.

On macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/aidin-foroughi/striso-live/main/install-mac.sh | bash
```

On Windows (PowerShell):

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

The Striso instrument and its original firmware, hardware and documentation are by **Piers Titus van der Torren** and contributors ([striso.com](https://www.striso.com)): all the credit for the instrument goes to them. This fork's additions are by Aidin Foroughi, vibe coded with Claude Code. The firmware is licensed under the GNU GPL v3 or later, as the original is; see the source file headers.
