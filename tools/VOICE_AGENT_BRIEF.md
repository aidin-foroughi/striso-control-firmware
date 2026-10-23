# Brief: a new expressive voice engine for the Striso's internal synth

You are one of four agents, each building a different voice engine for the Striso board's
internal synth, in parallel. The user can't describe what they want until they hear it: they
will play each voice on the instrument as soon as it is ready and pick favourites. Your job:
make your voice type sound as good and as playable as you can, safely, within the budgets below.
You can't listen to the board; you can render audio on the PC and analyse it.

## Hard rules

1. **Never touch the hardware.** Do not run `tools/striso.py`, do not flash, do not access USB
   devices or the UF2 drive. The coordinating session flashes your build after reviewing it.
2. **Stay in your lane.** Work only in your worktree (`../worktrees/fw-<voice>`, branch
   `voice-<voice>`). Outside it you may only *create new files*: `tools/harness/out/<voice>_*`
   (renders) and `tools/harness/perf_<voice>.h` (your own test performance). Don't modify shared
   tools or other worktrees. If you need a change there, say so in your report.
3. **Prefer adding files** in your worktree: `voice_<voice>.h` (+ helper headers if you like).
   Avoid editing shared firmware files (synth_strisy.cpp, Makefile, strisy.h, synth_control.cpp);
   if one is essential keep it minimal and explain it in the commit message and report.
4. **The build must pass the guards:**
   `FW_DIR=../worktrees/fw-<voice> tools/build.sh uf2 VOICE=<voice>` (run from the root of the
   main firmware checkout, `striso-control-firmware`) must end with `build checks passed`. It rejects software
   floating point helpers, synth stacks that don't fit, wrong load addresses.
5. **No libm in the engine** (sinf, cosf, expf, powf, tanf, logf, ...): newlib here does parts of
   them in software double precision; the build is rejected. Use polynomial/rational
   approximations (see strisy.h: fastpow2, tan0, svf_pade; voice_sine.h: cos/sin polys).
   Use `f` suffixes on float literals; never use double.
6. **Numerically safe, always.** No NaN/Inf output, ever, for any control values in range, with
   fast/extreme gestures, all voices at once, sudden note jumps. Physical models must be stable
   (passive) for all parameter combinations. Add a cheap per-voice guard: once per block, if a
   voice's state or output is non-finite or absurdly large, reset that voice's state.
7. **Level.** Similar loudness to the existing synth: under the default scripted performance its
   output peaks around 2 to 5 (RMS around 0.5 to 1). Never exceed about 8 even under stress.
   (The firmware multiplies by (volume/127)^2, about 0.25 at the usual volume, then hard clips
   at +-1, so the user would hear harsh clipping if you are much louder.)
8. **Commit your work** on your branch when done (and at sensible milestones). End commit
   messages with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`

## The board

- STM32H743, Cortex-M7 at 400 MHz, single precision FPU (double is software: avoid), 16 KB I/D
  caches. Sample rate **48828 Hz**, block **32 samples**, mono output, **6 voices** (VOICECOUNT).
- **CPU budget:** worst case (6 voices sounding, all controls moving) at most **50% CPU** =
  131k cycles per block, i.e. about **20k cycles per voice per block (~650 cycles per voice-sample)**.
  Measured on this chip: ~1.5 to 2 cycles per FP instruction in real loops, divisions ~14+
  cycles, DTCM loads 1 cycle. So aim for **at most ~300 FP ops per voice-sample** and estimate
  your cost in your report (the coordinator measures the real number on the board).
  Techniques that worked (see strisy.h): skip silent voices; compute coefficients at a control
  rate (every 8 samples) and interpolate; no per-sample divisions/transcendentals; keep the
  per-sample loop's state in locals; split long dependency chains into separate loops.
- **Memory:** the `Engine` object is placed in DTCM (128 KB, zero-wait) - keep it under ~90 KB.
  It must be plain data: zero-initialized, no constructors run; `init()` sets everything.
  Bigger buffers (delay lines) can be separate `static` arrays in your header (normal cached
  AXI SRAM, plenty of space).
- **Stack:** the synth thread has 4 KB; don't put big arrays on the stack.

## The API (see the comment at the top of `synth_strisy.cpp`, and `voice_sine.h`)

In `voice_<voice>.h`, namespace `<voice>`: `typedef float real;`, `BLOCK` (= 32), `MAX_VOICES`
(>= 6), `struct Engine` with members `strisy::GlobalControls g; strisy::VoiceControls vc[MAX_VOICES];
int nvoices;`, and `init(Engine&, int sample_rate, int nvoices)`, `process(Engine&, real* out)`
(writes BLOCK mono samples), `active_voices(const Engine&)`. Include `strisy.h` for the control
structs and helpers. Mark functions `static inline`.

## What the controls mean (per voice: `e.vc[v]`, global: `e.g`; read once per block)

- `note`: MIDI note number, float, continuous. Already includes pitch bend from sideways key
  movement. Small changes while a key is held are vibrato/glides; a jump of 1 semitone or more
  usually means the voice was reassigned to a new key: treat it as a new note.
- `pres`: key pressure, 0 when released, typically 0..1 (may slightly exceed 1). The Striso's
  main expressive dimension: continuous, fast, precise.
- `vpres`: pressure velocity, signed: a positive spike when a key is struck/pressed quickly,
  negative when released quickly; magnitude roughly 0..1.
- `but_x`: sideways tilt, -1..1 (also already applied as pitch bend in `note`).
- `but_y`: forward/backward tilt, -1..1.
- `g.acc_abs`: accelerometer magnitude, ~1 at rest, > 1 when shaking. `g.acc_x/y/z`: orientation
  -1..1. `g.rot_x/y/z`: gyroscope, -1..1, exactly 0 when the motion sensor is off. `g.pedal`:
  0..1 (default 3/16).
- Controls update asynchronously at ~1.3 kHz. A voice has `pres > 0` while its key is held;
  after release its last note stays until the voice is reused.

## Tools (run from the main firmware checkout `striso-control-firmware`, Git Bash on Windows, Docker)

The tools are in that checkout's `tools/`; the worktrees are next to it in `../worktrees/`.

- Build (~15 s): `FW_DIR=../worktrees/fw-<voice> tools/build.sh uf2 VOICE=<voice>`
- Host render (same engine code compiled for the PC):
  `FW_DIR=../worktrees/fw-<voice> VOICE=<voice> tools/harness/render_strisy.sh <voice>_<tag> [g++ flags]`
  -> `tools/harness/out/<voice>_<tag>.f32` (float32, 48828 Hz mono); prints peak and warns about
  non-finite samples. Default performance: `tools/harness/performance.h` (17.5 s: swelling note
  with strike, 6-voice chord with vibrato, tilt sweeps, shaking + rotation, pedal, fast strikes,
  extreme notes 20 and 115). Your own: write `tools/harness/perf_<voice>.h` (copy the structure of
  performance.h) and render with `PERF=perf_<voice>.h`.
- WAV for listening on the PC: `tools/.venv/Scripts/python tools/harness/compare.py X.f32 X.f32 --wav`
  writes `X.wav`. Python with numpy: `tools/.venv/Scripts/python`.
- Reference code: `strisy.h` (the current synth: structure, SVFs, control rate, gating),
  `voice_sine.h` (minimal API example).
- Shell quirks: heredocs piped to `python` collapse `\\` to `\` - write scripts and source files
  with the Write tool, not heredocs. Mind CRLF: the Makefile is CRLF; use the Edit/Write tools.

## Updates (2026-10-19), these override the text above where they differ

- **Base branch is now `voices`** (commit fb10f92 or later): all engines in one firmware
  (`make VOICE=all`), switchable at runtime. Build a single engine with `VOICE=<name>` as before.
- **10 voices** (VOICECOUNT is 10 for the C++ engines). Your `MAX_VOICES` must be >= 10 and the
  CPU budget is now for 10 voices: worst case at most ~50% CPU = 131k cycles per block, i.e.
  ~13k cycles per voice per block (~400 cycles per voice-sample, aim for <= ~200 FP ops).
- **Memory:** DTCM (Engine) has ~90 KB free in total, shared by all engines: keep your Engine
  <= ~20 KB. Big buffers: normal static arrays in AXI SRAM (~300 KB free, shared) or
  `__attribute__((section(".ram1_clear")))` in AHB SRAM1/2 (~130 KB free, shared). Say what you use.
- **Live parameters** (for the interactive app): expose the sound-design values of your engine by
  specializing `strisy::EngineParams<YourEngine>` (see strisy.h): `count`, `desc(i)` returning a
  `ParamDesc {name, group, min, max, kind}` (kind 0 continuous, 1 integer, 2 on/off), `ptr(e, i)`
  returning the live `float*` (so parameters must be stored as floats), and `changed(e, i)` to
  recompute derived values. Writes arrive between blocks, at any time, while voices sound: they
  must never cause NaN, blow-ups or pitch jumps of sounding notes. Names: short lowercase ids,
  unique, groups for UI sections (e.g. "osc", "filter", "amp", "accent", "mod").
  A preset change overwrites the parameters with the preset's values.
- **Stereo:** after the voice engine the master effects (`fx.h`, mono in, stereo out) run;
  voice engines stay mono.
- **Release behaviour:** after a key release the firmware leaves the voice's last `vpres` in place;
  ignore positive vpres while pres == 0 (test with tools/harness/perf_stale.h).
- `tools/harness/compare.py --wav` now writes WAVs scaled x0.25 like the board.
- The build's stack guard follows the call graph (tools/stackcheck.py).

## Updates (2026-10-19, later)

- Base is `voices` at c609602 or later: master effects (fx.h, about 10% CPU at defaults) and the
  electric piano (ep) are in. Remaining CPU budget for a voice engine: **at most ~45% at 10 voices**
  worst case (118k cycles per block, ~11.8k per voice per block).
- **Memory now:** ram1 (AHB SRAM1/2) is nearly full (~55 KB free): don't use it. Use normal static
  arrays in AXI SRAM: ~260 KB free in total, shared. The old `reed` voice (160 KB of delay lines)
  will be removed from the all-voices firmware when the new woodwinds engine lands.
- The host renderer takes `-DNV=10` (number of voices): test with 10.
- Presets are the way to offer different instruments (the user preferred realistic separate
  instruments as presets over continuous morphs). Level-match presets (median active RMS +-3 dB).
- The user plays and judges by ear; they value realism and subtlety.

## Updates (2026-10-22): sample rate 48000 Hz

- The board now runs at exactly **48000 Hz** (was 48828): the codec clock comes from its own PLL
  (cfg/mcuconf.h), for USB audio output. Engines get the rate in `init()`; never hard-code it
  (`e.sr`, `e.inv_sr`). The harness renders at 48000 Hz (performance headers, compare.py). The CPU
  budget per 32-sample block grows by 1.7% (266.7k cycles).

## Your report (final message)

1. What the voice is and how it works (a paragraph).
2. **How to play it**: what each control does, so the user knows what to try.
3. Estimated cost (FP ops per voice-sample, cycles per block worst case), memory use.
4. Host render results: peak/RMS on the default performance and your stress test, NaN-free.
5. WAV path(s) to listen to on the PC.
6. Branch + commit hash; any shared-file changes or tool requests.
7. Known limitations and ideas for a next iteration.
