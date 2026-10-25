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
| trumpet | static arrays still (below) | | |

The VOICE=all firmware is nearly full: AXI SRAM had about 31 KB free and AHB SRAM1/2 about 40 KB. A new engine with large buffers needs to check `tools/build.sh VOICE=all`'s memory report.

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

### MIDI 2.0: the Striso UMP profile

Everything that plays the internal synth reaches the engines as MIDI 2.0 Universal MIDI Packets, through one decoder (`striso_dsp/ump.h`, `ump::Decoder`):
- **the keys:** `synth_control.cpp`'s `update_synth_voice` sends each key's controls as packets, in the same key message, block and order as before;
- **MIDI 1.0 and MPE in:** `NoteInput` (`midi_in.h`) up-converts them;
- **MIDI 2.0 in:** `UmpInMsgHandler` (the virtual board's `sv_ump_write`; USB MIDI 2.0 next) gives MIDI 2.0 channel voice messages to `NoteInput::message2` and `InstrumentInput::message2`, and MIDI 1.0 ones in UMP (types 1, 2) to `MidiInMsgHandler`.

The engines and their controls did not change. The board's engine controls are bit for bit what they were: every control_replay golden, synth_control scenario, virtual transcript and render, and both board-vs-shell tests are unchanged.

**1. Inside and the edges.** Inside the instrument the packets are standard MIDI 2.0 channel voice messages (type 4, group 0, channel 0) with the Striso scale. The note number is the host's note id (on the board: a key 0..67, a MIDI in note 68..83), not a pitch.

| engine control | message | Striso scale (inside) |
|---|---|---|
| note on / off | Note On 0x9 / Note Off 0x8 | velocity 16; attribute type 0x02 (the profile), data bit 0 = a strike |
| note (pitch) | Assignable Per-Note Controller 0x1, index 1 | signed 9.23 semitones (−256..256) |
| pres | Poly Pressure 0xA | pres × 8191 × 2^17 (the board's code << 17), unsigned |
| vpres | Assignable Per-Note Controller 0x1, index 2 | vpres × 8191 × 2^16 (code << 16), signed |
| but_x | Assignable Per-Note Controller 0x1, index 3 | as vpres |
| but_y | Assignable Per-Note Controller 0x1, index 4 | as vpres |
| its sound ends (a newer note takes the id) | Per-Note Management 0xF, flag D (2) | |
| it ends silently (all notes off) | Per-Note Management 0xF, flag S (1) | |
| an exact value | Assignable Per-Note Controller 0x1, index 0x11..0x15 (pitch, pres, vpres, x, y) | the float32's bits |

A value decodes as `(float)(word / scale)` in double, one rounding. The board's codes (int16, `code / 8191`), MIDI 1.0's values (`v × (1/127)`, CC74, the release) and MPE's pitches all come back exactly. A value made from several keys (the portamento and free-transpose merges are weighted averages) can need more bits than 32: the encoder sends it as an **exact value** instead, so the engines get the same float. `sdk/tests/test_ump.cpp` checks every int16 code.

At every external edge (`sv_ump_write` now; USB MIDI 2.0 in and out next) the messages are converted to and from the standard MIDI 2.0 meaning.

**2. Standard input.**
- Full-scale pressure (2^32 − 1) is the Striso's calibrated full press: 1.0, code 8191 (not the 4.0 the inside scale has room for). The same holds for CC1.
- Note velocity is 16 bits: `v / 65535`, a release with a negative vpres.
- Bipolar controllers (pitch bend, CC74) follow the spec: the centre 0x80000000 is exactly 0, and each side is scaled to its own end.
- Registered Per-Note Controller 3 (Pitch 7.25) sets a note's own pitch. Inside it becomes 9.23.
- Registered Controller 0 is the bend range (semitones and cents), Registered Controller 6 is the MCM, and CC64 is the pedal.
- The board's remote-control CCs and its echo filter take the value's top 7 bits.

`virtual.ump in: ...` sends one performance three ways: as MPE bytes, as standard MIDI 2.0, and as MIDI 1.0 in UMP. Where MPE can say a value, the engine controls are the same, bit for bit: pressures, CC1, CC74, bends, the bend range, the pedal, velocity 127, the releases. Where MPE can't, they follow the finer value: 16-bit velocities, 32-bit pressures, a bend finer than 14 bits, a note's own pitch.

Two things MIDI 1.0 says differently from its closest MIDI 2.0 value:
- **Velocities.** v/127 has no exact 16-bit word, so it is up to 7.6·10^−6 away.
- **Eight 7-bit values** (9, 13, 18, 26, 36, 52, 72, 104). Here MIDI 1.0's float `v × (1/127)` is one float step off v/127. The 32-bit value gives the correctly rounded v/127, and the MIDI 1.0 path keeps its own product.

**3. Touch factors.** The decoder applies them per note, in the board's float expressions: `pres × pres_sensitivity`, `vpres × velo_sensitivity`, `clamp(y × y_sensitivity, −1, 1)` for a key, and × 1 for a MIDI in note. The packets carry the untouched values.

**4. Out-of-range pitch.** MPE pitches outside 0..127 (−48..175 with a 48-semitone bend) are exact inside under 9.23. They are clamped only at the MIDI 2.0 output edge, to Pitch 7.25's 0..128.

**The strike.** A Note On with the strike bit gets its vpres faded by the decoder: ×0.6 every 2 ms (`tick()`) while above 0.01, then 0. This is what MIDI in always did. The keys send their measured vpres and no strike bit.

**MIDI 2.0 out.** The MIDI mode setting (`Mmode`) has four values: `mpe`, `normal`, `mono` and `midi2`.
- **mpe, normal, mono.** These are the same bytes as before on DIN and on USB (every MIDI-out test and control_replay golden is unchanged).
- **midi2.** It is MPE on MIDI 1.0, plus the keys as standard MIDI 2.0 (`striso_dsp/ump_out.h`, `StandardOut`) on USB MIDI 2.0.
- **The MIDI 1.0 out is not a down-conversion of the internal UMP.** The MIDI out has its own logic: channels per voice, `message_interval`, change detection on 7/14-bit values, the configurable CCs, continuous velocity. The internal packets don't carry any of it, so a down-conversion would be a second copy of the MIDI out. The MIDI out stays as it was.

The MIDI 2.0 messages of a key (group 1, channel 1):
- Note On: velocity 16, with attribute 3 (Pitch 7.9).
- Registered Per-Note Controller 3 (Pitch 7.25), right after the Note On.
- Poly Pressure (1.0 = 0xffffffff).
- Registered Per-Note Controller 74 (brightness: MPE's CC74, from y).
- Assignable Per-Note Controllers 3 (x) and 2 (vpres).
- Note Off with the release velocity.

How the output edge converts:
- Values are clamped to the standard ranges: pres and the velocities 0..1, the bipolar ones −1..1, the pitch to 0..128.
- The exact values are converted too.
- Only a changed 32-bit value is sent again.
- A note's number is the nearest free one to its pitch. The Striso's C♯ and D♭ are two keys of one 12-TET note; the real pitch is the per-note pitch.

**USB.** Interface 1 has two alternate settings:
- **alt 0:** USB MIDI 1.0, as before.
- **alt 1:** USB MIDI 2.0. It uses the same endpoints and one Group Terminal Block: bidirectional, group 1, MIDI 2.0 protocol.

A host with a MIDI 2.0 driver selects alt 1: macOS 14+, Windows MIDI Services, Linux 6.5+. Any other host keeps alt 0.

The rule when the setting and the alt setting disagree: **the setting picks what is said, the alt setting only how it travels.**

| | USB alt 0 (MIDI 1.0) | USB alt 1 (MIDI 2.0) | DIN |
|---|---|---|---|
| `mpe`, `normal`, `mono` | the MIDI 1.0 stream | the same bytes as MIDI 1.0 in UMP (types 1, 2) | the MIDI 1.0 stream |
| `midi2` | MPE | the keys in MIDI 2.0. The rest of MPE's stream (motion CCs, program changes, the MCM) is MIDI 1.0 in UMP; its note data is not. | MPE |

In every case the board says something useful, and nothing is said twice. MIDI in on alt 1 takes UMP (`UmpInMsgHandler`, as `sv_ump_write`).

`virtual.usb midi2: ...` covers the rule, the descriptors and the input. `target.key_path` measures the MIDI 2.0 out: 3594 extra instructions per block for 10 moving keys.

**Not in it yet.** The globals have numbers (the pedal CC64; the accelerometer and gyroscope, Assignable Controllers 0x10..0x16), but the board still writes them directly.

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

#### MIDI in and the voices

Three pure units (no HAL, no heap), with the profile's decoder (`ump.h`), are the one copy of how MIDI in plays the internal synth. The board's `synth_control.cpp` runs them, and so does the SDK (`sdk::MidiVoices`, used by the shell and the renderer's MIDI files).

**`striso_dsp/midi_in.h`, `NoteInput`: the notes.**
- The MPE lower zone: per member channel, its bend times the bend range, pressure and CC74.
- The other channels' notes, with poly and channel pressure, CC1 and CC74.
- Velocity becomes a strike that fades ×0.6 every 2 ms; note-off velocity becomes the release.
- With 16 notes held, a new one takes the oldest.
- It is the up-converter into the Striso UMP profile (above): the host gets packets (`ump`) for its decoder, which then calls `note_started`, `note_changed`, `note_silenced`, `note_dropped`. The host also says which channels its own MIDI out used lately (`echoed`) and gives the bend range.
- `message2` takes standard MIDI 2.0 the same way, at its resolution.

**`striso_dsp/midi_in.h`, `InstrumentInput`: the instrument's messages.** It does:
- the RPN select;
- the notes' bend range (RPN 0: at least 1, not the zone master channel's);
- the MCM (RPN 6 on channel 1: the zone's members, at most 15);
- the instrument's bend (pitch bend outside the zone, ±2 semitones);
- the pedal (CC64).

The rules are the unit's. What follows from them is the host's: the board reconfigures its MIDI out on an MCM, moves its keys' `note_offset` with the bend, and knows which channels carry its own bends (echoes). The board's remote-control CCs (volume, modes, intervals) stay in `synth_control.cpp`.

**`striso_dsp/voice_allocator.h`, `VoiceAllocator`: which note plays which voice.**
- **The rule, in order:**
  1. a repeated note keeps its voice;
  2. else a voice no note has had;
  3. else the quietest released voice;
  4. else the quietest held voice, if it is quieter than the new note;
  5. else none.
- **The level estimate:** set at the note-on (pressure + strike), raised to its floor, and decaying ×0.998 − 0.0005 per 2 ms tick.
- **What a note is:** a number. It knows no MIDI channels: on the board it is a key or a MIDI in note's slot, the UMP profile's note id.
- **The host's part:** it says which notes are held and gives their level floor. Voices below `first` are the host's (the board's Mono mode).

MIDI is taken in 2 ms steps, as on the board: the voices' level decay and the strike fade first, then the step's messages.

`virtual.midi in: ...` feeds MIDI files through both the board and the shell, and requires bit-identical engine controls:
- an expressive file: MPE and master-channel notes, bends, pressures, the pedal;
- a crowded one: 16 notes over 10 voices, so released and held voices are taken.

## Tests

| | |
|---|---|
| `contract.<engine>.*` | `sdk/tests/contract.cpp` on every engine. Info; init (silent, idle, a note sounds); release (silent within the declared tail, in every preset); reset; determinism; independence of instances; fuzz of controls and parameters in every preset; the parameter table and write rules; presets and switching while notes sound; voice stealing (note jumps, instant re-strikes, every voice at once); undeclared inputs make no difference; CPU smoke. |
| `contract.fx.*` | The same for the master effects. |
| `sdk.*` | The helpers (fastpow2, tan0, the SVF, write_param, track_key, EngineSet), the MIDI mapping and files, the shell. |
| `striso_render.*` | The log names engine, preset and input; bad arguments are refused. |
| `render.*`, `target.*`, `virtual.render.*` | The golden checksums: host, QEMU (the board's arithmetic) and through the virtual board's firmware. All three go through the contract. |

Known exception: `contract.trumpet.independence` is expected to fail (doctest `should_fail`). `voice_trumpet.h` still keeps its delay lines in static arrays (`cyl_f`, `cyl_b`, `room_buf` in `.ram1_clear`), and its `dbg_*` calibration variables are globals, so two trumpet instances share them. One trumpet at a time, as on the board, is unaffected. Once the trumpet keeps the contract the test "passes", so doctest reports it as failing: then remove `shares_static_memory()`'s entry in `contract.cpp`.

## Porting notes

### The trumpet rewrite (Track B, `trumpet-phys`)

`voice_trumpet.h` was not touched; `engines/trumpet.h` adapts it as it is.

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
