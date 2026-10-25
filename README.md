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

The Striso is now also a **class-compliant USB sound card**, called **"Striso Synth"** (48 kHz stereo, 24 or 16 bit). It needs no driver on Windows 10/11, macOS or Linux. You can record the internal synth straight into any DAW or hear it through your computer, while the headphone jack keeps playing at the same time. The audio clock was retuned to run at exactly 48 kHz. It is a USB Audio Class 2.0 device, so Windows uses its low-latency driver (`usbaudio2.sys`), and the board adds about 2.5 ms on its side. The design, descriptors, latency budget and diagnostics are in [doc/usb_audio.md](doc/usb_audio.md).

### Striso Live, the optional companion app

[**Striso Live**](https://github.com/aidin-foroughi/striso-live) is a free desktop app for Windows and macOS. Everything in it is optional, but it makes the new sounds easy to explore. With it you can:

- turn every sound and effect parameter live while you play;
- save your own sounds, and A/B them against the preset;
- see scales (66 of them, including bebop and Barry Harris), chord names and roman numerals on the board as you play;
- set touch sensitivity and even out individual keys, then save them to the board;
- hear the Striso through your computer and record it to WAV;
- play full screen in stage mode.

## What this fork adds, in detail

- **Eight voice engines on the board, with presets:**

  | Settings key | Voice | Presets |
  |---|---|---|
  | grey | **strisy** | the original sound, rewritten in C++ (about 5× less CPU) |
  | orange | **VA** (virtual analog) | classic, pad, lead, bass, brass, keys, acid, strings |
  | blue | **modal** (bars and bells) | vibraphone, marimba, glockenspiel, bell, kalimba, wine glass |
  | red | **bowed** (strings) | violin, viola, cello, bass, section, ponticello, flautando, sordino, pizzicato |
  | green | **wind** (reeds and flute) | clarinet, bass clarinet, soprano/alto/tenor/baritone sax, oboe, bassoon, flute |
  | purple | **EP** (electric pianos) | stage, suitcase, mk2, dyno, ballad, bark, neosoul, fm80s, lofi, wurli, wurlibark, pianet, felt, celesta |
  | amber | **brass** | trumpet, flugelhorn, trombone, horn, euphonium, tuba, harmon, plunger, section |
  | cyan | **trumpet** (physical model) | Bb classical, Bb jazz, C trumpet, piccolo, cornet, harmon, cup mute, straight, plunger |

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

The firmware is a CMake project (CMake 3.25 or newer, presets in `CMakePresets.json`). The easy way builds in Docker, with the toolchain pinned in `tools/build-env/Dockerfile`: the Arm GNU Toolchain 14.2.rel1, CMake 3.31.6, Ninja, Faust 2.37.3 and QEMU (for the target renders below). The archives are checksum verified.

```sh
git submodule update --init
tools/build.sh VOICE=all          # all voices; VOICE=strisy (default) or VOICE=<name> builds one
tools/striso.py flash build/firmware/striso_control.uf2
```

The firmware lands in `build/firmware/striso_control.uf2`, next to the `.elf`, `.bin`, `.hex` and `.map`. Other options: `SYNTH_ENGINE=faust`, `PRESET=firmware-debug` (`-Og`, no LTO, the synth DSP still `-O3`), `SYNTH_OPT="..."`, or any `-D<CMake option>`. The old `make`-style arguments (`uf2`, `clean`) still work. Each run configures from its arguments, so an option you leave out goes back to its default, as with make.

The build checks the firmware. If a check fails, the build fails and no `.uf2` is written, so it can't be flashed. It checks that:

- no soft-float helper code got in (the DSP must use the FPU);
- the synth thread's stack is large enough (a call-graph check);
- the load address is right.

The version string the board reports is `<commit date>_<git describe>[_<voice>]`, e.g. `2026-10-23_v2.2.4-80-gf36b997_all`. `tools/striso.py flash` checks it after flashing.

Without Docker you need cmake, ninja, the arm-none-eabi toolchain, python3 and git, plus faust for the Faust engine:

```sh
cmake --preset firmware -DSTRISO_VOICE=all && cmake --build --preset firmware
make VOICE=all                    # the same, through the thin Makefile wrapper (also prog_*, gdb)
```

CMake options: `STRISO_VOICE` (strisy, all, or a name), `STRISO_SYNTH_ENGINE` (strisy, faust), `STRISO_SYNTH_OPT`/`STRISO_SYNTH_LTO` (flags of the synth DSP file), `STRISO_FAUST_REGENERATE`, `STRISO_USB_AUDIO_PRE_VOLUME`, `STRISO_KERNEL_STATS` (ChibiOS lock-section statistics for `StcU`, on by default), `STRISO_LISTING`. Every build exports `compile_commands.json` for clangd and IDEs.

### Tests

```sh
tools/test.sh                     # host tests (ctest --preset host), in Docker
tools/test.sh host-asan           # the same under AddressSanitizer and UndefinedBehaviorSanitizer
```

The host tests render every engine and preset on the PC: the default, stale-input and recorded performances (`tools/harness/rec`). They check that the output is finite, the peak is bounded and released voices fall silent. Each render is compared with a golden checksum (`tests/golden/renders.txt`), so any change to a voice's sound shows up. Unit tests (doctest) cover the live-parameter fuzz, the bowed engine's bounds, the config record format, the tuning math and `tools/striso.py`. The USB audio core has a simulation test.

`ctest --preset firmware` (after building the firmware and `--target target_renders`) checks the USB descriptors and the UF2. It also runs every engine compiled for the board's CPU under QEMU, as a Cortex-M7 with the board's FPU settings, and compares the output with `tests/golden/target_renders.txt`. That shows what a compiler change does to the board's arithmetic. After an intended change to the sound, `tools/update_goldens.sh` rewrites both golden files.

`tools/format.sh` (clang-format) and `tools/tidy.sh` (clang-tidy) cover our C++. CI (`.github/workflows/firmware.yml`) runs all of it on every push, with the virtual board's tests on Windows and macOS, and publishes a release for every `v*` tag (see Releases).

### Testing without the hardware: the virtual board

`virtual/` builds the firmware's logic as a library for the PC, **striso_virtual** (`.dll`, `.dylib`, `.so`), from the board's own source files: `synth_control.cpp`, `synth_strisy.cpp` with every engine (as `VOICE=all`), the USB console and its protocol (`pconnection.c`), the message path (`messaging.cpp`, `control_thread.cpp`), `config_store.c`, `led.c`. They run on a small ChibiOS/HAL stand-in (`virtual/hal`): the firmware's threads run one at a time by priority, in virtual time (the 2 kHz tick, the codec's 32-frame block interrupt, the 1 ms USB frame), so a run is deterministic. What only the hardware has is replaced (`virtual/board`): key and control messages are injected where the key scanner sends them, so the firmware's own `synth_message` path runs; motion where the sensor thread reads it; the USB bulk console is a pair of byte queues with exactly the board's protocol (`Stc` commands, Q writes, the S/s binary stream). StcB opens the bootloader's drive as a directory, and a UF2 written there is flashed into the virtual board's config. No firmware file is changed for it; the board build is the same.

```sh
tools/virtual.sh            # build and test natively (Windows: clang with Visual Studio's libraries; macOS: Xcode's clang)
UNIVERSAL=1 tools/virtual.sh                                       # macOS: arm64 + x86_64 in one library (build/host-virtual-universal)
python3 tools/ctest_arch.py build/host-virtual-universal arch -x86_64   # its x86_64 slice's tests, under Rosetta
cmake --preset host-virtual && cmake --build --preset host-virtual && ctest --preset host-virtual
```

The library is `build/host-virtual/virtual/striso_virtual.*`; its API is `virtual/include/striso_virtual.h` (boards, render and taps, real time, the console and USB MIDI, key/control/motion injection, LEDs and state, the flash). Its tests (`ctest -L virtual`, also part of `tools/test.sh`):

- every golden render of the firmware's 56 voices (default, stale and recorded performances, the effects) through the board's synth thread, bit for bit with `tests/golden/renders.txt` (Windows x64, macOS arm64 and x86_64, Linux);
- recorded key streams through the firmware's message path, replayed on the straight engine: bit for bit;
- voice switches leave nothing behind (an engine renders its golden after another ran);
- console transcripts (`virtual/tests/transcripts`) and the real board's replies to the read-only commands, field for field (`transcripts/board`, recorded from a Striso);
- the API: sound, LEDs, resets, the bootloader drive and its config, determinism, real time.

Striso Live uses it as its **Virtual** Striso, and to audition a build with a real Striso as the controller (`--virtual-lib=<build>/virtual/striso_virtual.dll`): see its README. `test_mirror.py` keeps the virtual board's copies of `main.c`'s and `motionsensor.c`'s loops equal to the firmware's.

### Releases

A `v*` tag publishes a GitHub release (CI's `release` job, after every test has passed):

| File | What |
|---|---|
| `striso_control_<version>.uf2` | the default firmware (strisy); `<version>` is what the board reports, e.g. `2026-10-24_v2.3.0-0-gabc1234` |
| `striso_control_<version>_all.uf2` | the firmware with every voice engine (`VOICE=all`) |
| `striso_virtual-<tag>-windows-x64.zip` | the virtual board for Windows x64 (`striso_virtual.dll`); needs the Microsoft Visual C++ 2015-2022 runtime, 14.28 or later |
| `striso_virtual-<tag>-macos-universal.zip` | the virtual board for macOS 11 and later, Apple silicon and Intel (`libstriso_virtual.dylib`, arm64 + x86_64) |
| `SHA256SUMS` | `sha256sum -c SHA256SUMS` |

Each zip holds the library, `striso_virtual.h`, `COPYING` and `manifest.json`: the release version, the firmware version and the C API version the library itself reports (`sv_firmware_version`, `sv_api_version`), its sha256, the commit, what it links and the minimum OS. Striso Live loads the virtual board at run time and does not ship it. The macOS library is built with `UNIVERSAL=1 tools/virtual.sh` and its tests run on both slices; `tools/package.sh` loads each slice and refuses a library that links anything outside the system.

`tools/release_dryrun.sh` makes the Windows parts on a PC into `dist/`, with the same scripts and tests (`--no-tests` to skip them): both UF2s, the Windows zip and `SHA256SUMS`, then checks them (`tools/package.sh check`: the sums, every UF2 block, each zip against its manifest). Nothing is pushed or tagged.

### Layout

| Path | What |
|---|---|
| `src/app` | main, the USB console and protocol (pconnection), the message path (the typed event queue, the control thread, the wire frame), configuration, the config editor page |
| `src/drivers` | codec, key scanning (ADC), motion sensor, LEDs, aux jack, serial MIDI |
| `src/usb` | USB descriptors, bulk console, USB MIDI, USB audio |
| `src/synth` | the synth thread and controls (`synth_strisy.cpp`, `synth_control.cpp`), the tuning math; `faust/` the Faust synth |
| `dsp/include/striso_dsp` | the DSP library: strisy, the voice engines (`voice_<name>.h`), the master effects. Header only, C++20, no HAL |
| `board`, `cfg`, `ld` | board definition, ChibiOS configuration, linker script |
| `cmake` | toolchain file, ChibiOS integration (from its own make fragments), version, post-build checks |
| `tests` | host tests and golden checksums |
| `virtual` | the virtual board: the firmware's logic as a PC library (striso_virtual), its HAL stand-in and tests |
| `tools` | PC tools, the Docker build environment, the renderers |
| `third_party` | ChibiOS and uf2 (submodules), doctest |

In a checkout from before this layout, `git submodule update --init` puts the submodules under `third_party/` (the old top-level `ChibiOS/` and `uf2/` directories can then be deleted). Blame skips the formatting commit when you run `git config blame.ignoreRevsFile .git-blame-ignore-revs`.

## Tools

`tools/` holds the PC-side tooling. Install the Python dependencies with `python -m venv tools/.venv && tools/.venv/Scripts/pip install -r tools/requirements.txt` (on macOS/Linux: `tools/.venv/bin/pip`).

- `tools/striso.py`: talks to the board over USB:
  - read the version, CPU load and profile;
  - list and set live parameters (`params`, `set`) and select voices (`voice`);
  - save touch settings (`save-touch`);
  - flash safely (`flash`). It backs up first, refuses blocks outside the app region and health-checks the board after the reboot.
- `tools/harness/`: renders the voice engines on the PC from scripted or recorded performances (`VOICE=<name> tools/harness/render_strisy.sh <name> "" --preset 2 --rec /src/tools/harness/rec/<take>.csv`). It has stress and stale-input tests and a test of the master effects (`fx_test.sh`, `fx_suite.sh`). `cmake --build --preset host --target render_wavs` writes a WAV of every engine. `tools/harness/rec/` has real key-stream recordings for replay.
- `tools/target_render/`: the engines compiled for the board's CPU and run under QEMU (see Tests).
- `tools/virtual.sh`: builds and tests the virtual board natively (see Testing without the hardware).
- `tools/package.sh`, `tools/release_dryrun.sh`: the release files and a dry run of a release (see Releases). `tools/ctest_arch.py` runs a build's tests under a launcher (`arch -x86_64`). `tools/ci_ccache.sh`, `tools/ci_docker.sh`: CI's ccache and its runs in the build image.
- `tools/build.sh`, `tools/test.sh`, `tools/update_goldens.sh`, `tools/format.sh`, `tools/tidy.sh`: run in Docker (see Building).
- `tools/record_keys.py`: records the key stream from the board to CSV.
- `tools/review_voice.sh`: builds and checks a voice engine before it may be flashed.
- `tools/usbaudio/`: USB audio descriptor checks (UAC2), a simulation of the audio buffer and clock-rate matching (servo, latency per ring target), and a test-tone analyzer. On the board: `striso.py cmd U` prints the stream statistics, `cmd u` toggles a test tone, `cmd t096` sets the ring target (see [doc/usb_audio.md](doc/usb_audio.md)).

## Credits and licence

The Striso instrument and its original firmware, hardware and documentation are by **Piers Titus van der Torren** and contributors ([striso.com](https://www.striso.com)): all the credit for the instrument goes to them. This fork's additions are by Aidin Foroughi, vibe coded with Claude Code. The firmware is licensed under the GNU GPL v3 or later, as the original is; see the source file headers.
