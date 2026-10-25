# Sound engines: the contract

Every sound engine of the Striso (strisy, va, modal, bowed, wind, ep, brass, trumpet, reed, sine) and the master effects keep one contract. The firmware is one host of it, a program on the PC another. An engine can be built, tested, rendered and played without the firmware.

- **The contract:** `dsp/include/striso_dsp/engine.h`. It has the `striso::dsp::VoiceEngine` and `striso::dsp::Effect` concepts, `VoiceControls`, `EngineInfo` and the memory placements.
- **Each engine as a VoiceEngine:** `dsp/include/striso_dsp/engines/<name>.h` is `striso::dsp::engines::<name>`. The master effects are `effects/fx.h`, `striso::dsp::effects::fx`.
- **The board's host:** `src/synth/synth_strisy.cpp`.
- **The PC's host:** `sdk/`. It has the engine libraries, the contract test suite, the renderer and the host shell.

## The contract

An engine is a plain-data object. Zero-filled storage is a valid engine before `init()`: on the board it lives in zero-filled DTCM, and no constructor runs. It has no globals, no heap, no HAL and no ChibiOS, and it makes no libm calls on its hot paths (newlib does parts of `expf`, `powf` and `tanf` in software double precision). Its large buffers are memory regions that the host gives it. A block is `BLOCK` = 32 samples.

| | |
|---|---|
| `init(fast, large, sample_rate, voices)` | Start from silence. The regions stay the engine's until the next `init`. `voices <= info.budget.max_voices`. |
| `reset()` | Back to silence. The preset and the live parameter values are kept. Afterwards it plays exactly as a fresh engine with that preset and those values. |
| `process(vc, g, out)` | One block. `vc[voices()]` are the voices' controls and `g` the globals. `out[0..channels)` gets `BLOCK` samples each, at about ±4 full scale (the board's volume is ×0.25 and it clips at ±1). |
| `active_voices()`, `voices()` | The voices sounding now, and the count `init()` set. |
| `num_presets()`, `preset_name(i)`, `set_preset(i)`, `preset()` | Presets. `set_preset` works at any time between blocks. `preset()` is -1 for an engine without presets. |
| `param_desc(i)`, `param(i)`, `set_param(i, v)` | Live parameters. `param_desc` is `nullptr` past the last one. `set_param` is a write by `write_param`'s rule: NaN is ignored, the value is clamped to [min, max] and rounded unless continuous. Then the engine's `changed()` runs. |
| `info` | An `EngineInfo` (below). |

An `Effect` is the same, with `init(fast, large, sample_rate)` and `process(in, out)`. `EffectInfo` gives its channel counts.

### Controls

`striso::dsp::VoiceControls` is one voice:

| field | |
|---|---|
| `note` | Pitch in semitones, in MIDI numbering (69 = A4). It is fractional, with bends and tunings included. |
| `pres` | Pressure: 0 is released and 1 is firm. It goes up to about 1.5. |
| `vpres` | The pressure's velocity, about -1 to 1. It is above 0 while the key goes down (the strike) and below 0 while it comes up (the release). |
| `but_x`, `but_y` | Tilt along and across the row, -1 to 1. synth_control has already bent `note` by 0.5 x³. |
| `gate` | 1 while the key is held, else 0. |
| `strike`, `release` | The velocity (0 to 1) of the strike that opened the gate and of the release that closed it. Each is kept until the next. |

The first five are the board's key signals (`strisy::VoiceControls`): this is what synth_control writes. `striso::dsp::track_key()` derives `gate`, `strike` and `release` from them once per block. A MIDI host sets them from the note-on and the note-off. The engines written for the board read the five key signals: their `Engine::vc` is `strisy::VoiceControls`. An engine that wants the notes declares `striso::dsp::VoiceControls vc[]` and gets all eight.

`GlobalControls`:

- `acc_abs` and `acc_x/y/z`: the accelerometer in g. At rest it reads 1 g, pointing down.
- `rot_x/y/z`: the gyroscope, about -1 to 1.
- `pedal`: 0 to 1. At rest it is 3/16, the board's default.

### EngineInfo

- **name, summary.** The name is the firmware's, as the `v` console command lists it.
- **channels.** 1 or 2.
- **inputs.** The `Input` bits of the controls the engine reads. An input it doesn't declare must make no difference to its output; the contract suite checks this. Declaring an input it doesn't read is allowed (the suite only checks the other way). The pitch is always read.
- **budget.**
  - `max_voices` is a hard limit.
  - `instr_per_block` is the worst preset's QEMU instruction count per block: 10 voices, the default performance, `tools/target_render/profile.sh`. The board has 314k cycles per block.
  - `tail_seconds`: after the last release the engine is silent (below 1e-4) and idle within this time.
- **state_bytes, fast_bytes, large_bytes.** The engine object, its FastMemory and its LargeMemory.
- **num_presets, num_params.**
- **heroes, preset_heroes.** Up to 8 live parameter names, in display order: the knobs an app shows first. A list per preset can override the engine's, for presets whose character lives elsewhere (va's pad, bowed's pizzicato). `heroes_of(info, preset name)` gives the list that applies.
  - The effects have theirs too: fx's are `rvmix` and `dlmix`.
  - They were seeded from Striso Live's `hero.dart`. The contract suite checks that every name is one of the engine's parameters and every override names one of its presets.
  - The board sends them: `StcH` replies `H <voice> <name>... | <fx name>...`, the current voice's list (its preset's override, else its engine's), a bar, then the effects'. The app reads them from the board.

### Memory and where it goes

The host places three kinds of memory:

| | what | the board |
|---|---|---|
| `Placement::tcm` | The engine object: the hot state, small. | DTCM (`.ram5_clear`, 128 KB for every engine of VOICE=all, the effects and the stacks) |
| `Placement::fast` | `FastMemory`: large, touched every sample. | AHB SRAM1/2 (`.ram1_clear`, 256 KB) |
| `Placement::large` | `LargeMemory`: large, any RAM. | AXI SRAM (`.bss`, 512 KB) |

An engine without a region declares it `NoMemory`. On the PC, `striso::dsp::Memory<E>` holds both regions in one object, and `sdk::Engine` puts the engine and its memory on the heap.

What each engine needs:

| engine | state | FastMemory | LargeMemory |
|---|---|---|---|
| bowed | | strings, 100 KB | section delay, 8 KB |
| brass | | | bores, 80 KB |
| wind | | | 160 KB |
| reed | | | 160 KB |
| fx | | delay and pre-delay, 80 KB | reverb ring, 64 KB |
| trumpet | | bores of voices 0-7, 64 KB | bores of voices 8-9, 16 KB |

The VOICE=all firmware is nearly full: with the physical trumpet, AXI SRAM has about 10 KB free and AHB SRAM1/2 about 12 KB (neither has room for its 80 KB of bores alone, so they are split by voice). A new engine with large buffers needs to check `tools/build.sh VOICE=all`'s memory report.

## Writing an engine

1. Write `dsp/include/striso_dsp/voice_<name>.h`, in namespace `<name>`:
   - `using real = float;`
   - `BLOCK` and `MAX_VOICES`
   - `struct Engine { strisy::GlobalControls g; strisy::VoiceControls vc[MAX_VOICES]; int nvoices; int preset; /* yours */ FastMemory* fast; LargeMemory* large; };`
     - `preset` is needed only with presets; `fast` and `large` only with those regions.
   - `void init(Engine&, int sample_rate, int nvoices)`, which clears the regions it uses
   - `void process(Engine&, real* out)`
   - `int active_voices(const Engine&)`
   - optionally `NUM_PRESETS`, `preset_name(int)`, `set_preset(Engine&, int)` and a specialization of `strisy::EngineParams<Engine>` (live parameters)

   Include the helpers you use: `controls.h`, `params.h`, `dspmath.h`, `svf.h`. `voice_sine.h` is the minimal example.
2. Write `dsp/include/striso_dsp/engines/<name>.h`, the traits. Copy one; `engines/bowed.h` has both memory regions.
   ```cpp
   struct <name> {
       STRISO_ENGINE_FUNCTIONS(<name>)
       STRISO_ENGINE_PRESETS(<name>)        // with presets
       using FastMemory = ::<name>::FastMemory;   // or NoMemory
       using LargeMemory = NoMemory;
       static constexpr const char* name = "<name>", *summary = "...";
       static constexpr uint32_t inputs = in_pressure | in_velocity | ...;
       static constexpr Budget budget = {::<name>::MAX_VOICES, <instructions per block>, <tail seconds>};
   };
   ```
3. That is all for the PC. CMake finds `engines/*.h`, so the engine gets:
   - its library `striso_engine_<name>`;
   - the contract suite (`contract.<name>.*`);
   - a place in `striso_render --list`;
   - render tests for every preset (`render.<name>.*`), which need golden checksums (`tools/update_goldens.sh`).
4. For the board, add `E::<name>` to `SYNTH_ENGINES_ALL` in `src/synth/synth_strisy.cpp`, and its include above it. The order is the voice list's order (`v`, the settings keys). The virtual board's tests read the list from there. `tools/build.sh VOICE=<name>` builds a firmware with only that engine.

Then measure:
- `ctest -L contract -R <name>` prints the measured tail and the host CPU.
- `tools/target_render/profile.sh <name>` gives the instruction counts for the budget.

## Hosts

### On the board

`synth_strisy.cpp` is the board's host:
- **Placement.** `Engines::States` sits in DTCM, `Engines::Fast` in `.ram1_clear` and `Engines::Large` in `.bss`; the master effects are placed the same way.
- **Controls.** The keys' signals go to the engines' controls in one place: synth_interface points into `controls_vc` and `controls_g`, and `track_key` runs every block.
- **Dispatch.** `Engines::visit(k, ...)` is a chain of compares the compiler inlines. There are no function pointers, so the build's stack check follows every call.
- **Presets.** Every preset of every engine is one entry of the voice list.

### On the PC (`sdk/`)

| | |
|---|---|
| `striso_engine_<name>` | One engine as a table of functions (`sdk::EngineEntry`). `sdk::Engine` is an instance with its memory. |
| `striso_engines` | Every engine by name: `sdk::engines()`, `find_engine()`, `find_preset()`. |
| `striso_effect_fx` | The master effects: `sdk::fx_effect()`, `sdk::Effect`. |
| `striso_performances` | The scripted performances of `tools/harness`: `performance.h` is "default", `perf_stale.h` is "stale" and `perf_<x>.h` is "<x>". |
| `striso_sdk` | MIDI in (`midi_voices.h`, on the board's mapping `striso_dsp/midi_in.h`), Standard MIDI Files (`smf.h`), the players (`performance.h`: scripted, key CSV, MIDI file) and the shell (`shell.h`). |
| `striso_render` | The renderer (below). |

The SDK builds on its own, without the firmware tree or ChibiOS: `cmake -S sdk -B build/sdk && cmake --build build/sdk && ctest --test-dir build/sdk`. It needs `dsp/`, `tools/harness/` (the performances, the key-stream replay, the recordings) and `third_party/doctest`. On the firmware repository's host presets it is part of the build.

#### striso_render

```
striso_render --engine va --preset pad --perf default --wav va_pad.wav
striso_render --engine ep --preset 3 --rec tools/harness/rec/play_2026-10-22_legato.csv --fx --wav ep.wav
striso_render --engine bowed --midi tune.mid --wav bowed.wav
striso_render --list
```

The log on stderr says exactly what played:
- the engine and its header;
- the preset, by number and name, and whether it is the engine's default;
- the voices and the rate;
- the input: kind, name, absolute path, size, FNV checksum and length.

The WAV's INFO chunk says the same. An unknown engine, preset or performance, a file that can't be read, two inputs or too many voices all stop it with exit code 2. It never falls back to something else. stdout gives `hash <FNV-1a 64 of the float bits>`. `--check`, `--expect` and `--golden-name` make it the golden render test (`tests/CMakeLists.txt`).

`render_custom` is the same renderer with one engine and one performance compiled together with extra flags (test hooks, `-DBRASS_RL=...`). Use it through `tools/harness/render_strisy.sh`.

#### The shell

`sdk::Shell` is one engine plus the master effects, played by MIDI/MPE or by key signals. It renders any number of frames per call. A VST3 or CLAP plugin wraps it:
- `midi()` for its events;
- `process(left, right, frames)` for its audio callback;
- `engine()` for presets and parameters.

The engines run 32-sample blocks, and a MIDI event takes effect at the next block (within 0.67 ms at 48 kHz). A plugin that wants sample-accurate events splits its block at the events.

#### MIDI in

`dsp/include/striso_dsp/midi_in.h` (`striso::midi::NoteInput`) is the one MIDI note input mapping. The board's `synth_control.cpp` runs it, and so does the SDK (`sdk::MidiVoices`, used by the shell and the renderer's MIDI files):
- The MPE lower zone: its member channels (an MCM sets them); per channel, bend times the bend range (RPN 0), pressure and CC74.
- The other channels' notes, with poly and channel pressure, CC1 and CC74, and their bend: the instrument's ±2 semitones.
- Velocity becomes a strike that fades ×0.6 every 2 ms.
- Note-off velocity becomes the release.
- With 16 notes held, a new one takes the oldest.

It is pure: no HAL, no heap. Its host does three things:
- **Voices.** It gives the notes voices (`note_started`, `note_changed`, `note_silenced`, `note_dropped`). On the board these go to the key-voice allocator (`get_synth_voice`, shared with the keys); `sdk::MidiVoices` has its own allocator: a voice never used, else the one released longest ago, else none.
- **Echoes.** It says which channels were its own MIDI out lately (`echoed`), so their echo isn't played.
- **Bend range.** It gives the notes' bend range.

MIDI is taken in 2 ms steps, as on the board: the strike fade first if one is fading, then the step's messages. The instrument-level messages stay with the host: the bend range, the MCM, the instrument's bend and the pedal (CC64); `RpnSelect` is shared. The board's host also does its remote-control CCs.

`virtual.midi in: one MIDI file through the firmware and through the SDK's shell...` checks that the board and the shell give bit-identical engine controls for one MIDI file: ten notes, MPE and the master channel, bends, pressures, the pedal.

## Tests

| | |
|---|---|
| `contract.<engine>.*` | `sdk/tests/contract.cpp` on every engine. Info; init (silent, idle, a note sounds); release (silent within the declared tail, in every preset); reset; determinism; independence of instances; fuzz of controls and parameters in every preset; the parameter table and write rules; presets and switching while notes sound; voice stealing (note jumps, instant re-strikes, every voice at once); undeclared inputs make no difference; CPU smoke. |
| `contract.fx.*` | The same for the master effects. |
| `sdk.*` | The helpers (fastpow2, tan0, the SVF, write_param, track_key, EngineSet), the MIDI mapping and files, the shell. |
| `striso_render.*` | The log names engine, preset and input; bad arguments are refused. |
| `render.*`, `target.*`, `virtual.render.*` | The golden checksums: host, QEMU (the board's arithmetic) and through the virtual board's firmware. All three go through the contract. |


## Porting notes

### The trumpet rewrite (Track B, `trumpet-phys`)

Done in `trumpet-phys-sdk`: the bore delay lines are `FastMemory` (voices 0-7) and `LargeMemory` (voices 8-9), the test hooks compile only under `TRUMPET_TEST_HOOK`, and the contract passes (17/17, independence included). Renders are bit-identical to the static-array version. The steps, for reference:

1. Merge. Your header needs no change to build.
2. Move the delay lines into the contract's memory and drop the section attributes: the host places them now.
   ```cpp
   struct FastMemory {
       alignas(32) float cyl_f[NP][CLEN];
       alignas(32) float cyl_b[NP][CLEN];
       alignas(32) float room[ROOML];
   };
   ```
   Add `FastMemory* fast;` to `Engine`. Access `e.fast->cyl_f[i]`, best through a local `float* __restrict__` as the hot loops already do. In `engines/trumpet.h`, set `using FastMemory = ::trumpet::FastMemory;`.
3. Make the `dbg_*` globals (calibration, captures) members of `Engine`, or compile them in only under a macro.
4. Remove the `shares_static_memory()` entry in `sdk/tests/contract.cpp`.
5. Check:
   - `ctest -R contract.trumpet`: all pass, including independence;
   - the goldens: unchanged, or updated with `tools/update_goldens.sh` for a new model;
   - `tools/build.sh VOICE=all`'s memory report: ram1 holds the trumpet's 36 KB as before.
6. Set `budget.instr_per_block` from `tools/target_render/profile.sh trumpet`, and the tail from what the contract suite prints.
7. Check `heroes` and `preset_heroes` in `engines/trumpet.h` against the new parameter table. `contract.trumpet.heroes` fails on a name the table no longer has, or a preset that was renamed.

### The accordion (`voice-accordion`)

1. Keep `voice_accordion.h` as it is, with the free functions.
2. Add `dsp/include/striso_dsp/engines/accordion.h`. Copy `engines/va.h` (presets, no regions) and fill in the summary, the inputs it reads, the budget (`profile.sh accordion`), the tail and its hero parameters.
3. If it has large buffers, give them to it as FastMemory or LargeMemory, as in `voice_bowed.h`.
4. In `src/synth/synth_strisy.cpp`, add `#include "striso_dsp/engines/accordion.h"` and `E::accordion` to `SYNTH_ENGINES_ALL`. This replaces your change to the `VOICE_LIST` line and to `MAX_ENTRIES`: the entry count is computed now.
5. `ctest -R 'contract.accordion|render.accordion'` runs it. The goldens you have stay valid.

## Merge notes

- **The engine list.** `VOICE_LIST(X, XP)` is gone. A branch that edits it adds its engine to `SYNTH_ENGINES_ALL` instead, and adds an `engines/<name>.h`.
- **Firmware selection.** `SYNTH_VOICE_HEADER` is now `SYNTH_ENGINE_HEADER` (the traits header). `striso_voice_defines()` gives it.
- **Engine code with static buffers.** It needs the region pointers; see the trumpet notes.
- **Direct `ns::init` callers.** Code that calls `ns::init(e, ...)` directly on an engine with regions must set `e.fast` and `e.large` first. Better, it uses the adapter: `striso::dsp::engines::<name>` and `init(mem.fast, mem.large, sr, nv)`.
- **The renderer.** `render_<voice>` and `render_strisy.cpp` are gone; it is `striso_render --engine <voice> --perf <name>`. `render.<...>` keeps its test names.
- **Moved tests.** The parameter fuzz of `tests/test_params.cpp` is now part of the contract suites.
