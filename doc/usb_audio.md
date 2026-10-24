# USB audio: the synth as a USB sound card input

The Striso presents its synth as a class compliant USB audio input: 48 kHz stereo, 24 bit
(alt setting 1) or 16 bit (alt 2), on the asynchronous isochronous IN endpoint 0x83 at full
speed. The jack keeps playing; USB gets the same samples. Code: `src/usb/usb_audio.c` (USB
glue, diagnostics), `src/usb/usb_audio_core.h` (ring, servo, packing; tested on the PC by
`tools/usbaudio/test_core.c`), descriptors in `src/usb/usbcfg.c`.

## Class version: USB Audio 2.0 by default

The audio function is USB Audio Class 2.0 (UAC2). `-DSTRISO_USB_AUDIO_UAC1=ON` builds the
earlier UAC1 function instead (bcdDevice 2.01), for a host without a UAC2 driver.

| Host | UAC1 | UAC2 |
|---|---|---|
| Windows 10 1703+, 11 | `usbaudio.sys`, the legacy KS driver, with large internal buffering | `usbaudio2.sys`, a WaveRT miniport (the low latency path, exclusive mode) |
| Windows before 10 1703 | works | no class driver: no audio (MIDI and the console still work) |
| macOS (AppleUSBAudio) | works | works; the class Apple's own pro devices use |
| Linux (snd-usb-audio) | works | works |

So UAC2 is the better choice on Windows and costs nothing on macOS and Linux. Full speed UAC2
is common (TinyUSB devices, RP2040 boards, many USB microphones). Microsoft's driver page
(learn.microsoft.com, "USB Audio 2.0 Drivers") sets the rules the descriptor follows: one clock
source answering GET CUR and GET RANGE (a fixed clock needs no SET CUR), the same terminal link
in every alt setting, alt settings in ascending order from a zero bandwidth alt 0, exactly one
`bmFormats` bit, and packets within one frame of nominal (47/48/49).

The MIDI function (interfaces 0, 1) stays a USB Audio 1.0 MIDIStreaming function in both
builds: Windows loads `usbaudio.sys` for it, macOS and Linux their USB MIDI drivers, as with any
UAC2 device that has MIDI. Interface 2 (vendor bulk, WinUSB through the MS OS 1.0 compat ID)
and its endpoints are byte-identical in both builds. The app and `tools/striso.py` claim it.

`bcdDevice` is 2.02 for UAC2 and 2.01 for UAC1. Windows keys its hardware IDs (`REV_0202`) and
its MS OS descriptor cache (`usbflags\F05557C00202`) on it, so a class change gets a fresh
device node and driver binding instead of a stale one.

## Descriptors (UAC2, the audio function)

Interfaces 3 and 4, after the MIDI and vendor functions (configuration total 304 bytes).
`tools/usbaudio/check_descriptors.py --elf build/firmware/striso_control.elf` decodes and checks
all of it (ctest `usb_descriptors`); `--live` reads the board.

| Descriptor | Bytes | Fields |
|---|---|---|
| IAD | 8 | interfaces 3..4, class 01 / subclass 00 / protocol 20 (Windows: `USB\Class_01&SubClass_00&Prot_20` -> usbaudio2.sys), iFunction "Striso Synth" |
| Interface 3 alt 0 | 9 | AudioControl 01/01/20, no endpoints, "Striso Synth" |
| AC header | 9 | bcdADC 2.00, category 0x09 (musical instrument), wTotalLength 46, no latency control |
| Clock source, ID 3 | 8 | internal fixed clock (the codec's 48 kHz PLL), frequency control read only, validity read only |
| Input terminal, ID 1 | 17 | Line connector 0x0603, clock 3, 2 channels FL+FR, iTerminal "Striso Synth" |
| Output terminal, ID 2 | 12 | USB streaming 0x0101, source 1, clock 3 |
| Interface 4 alt 0 | 9 | AudioStreaming 01/02/20, zero bandwidth |
| Interface 4 alt 1, 2 | 9 | AudioStreaming 01/02/20, one endpoint |
| AS general | 16 | terminal link 2, Type I, PCM, 2 channels FL+FR |
| Type I format | 6 | alt 1: 3 byte subslots, 24 bit; alt 2: 2 byte subslots, 16 bit |
| Endpoint 0x83 | 7 | isochronous, asynchronous, data; wMaxPacketSize 294 (alt 1) / 196 (alt 2) = 49 frames; bInterval 1 |
| CS iso endpoint | 8 | no MaxPacketsOnly, no controls, no lock delay |

The configuration has no feedback endpoint: an IN stream's rate is in its packet sizes, so
an asynchronous source needs none. It has no feature unit either. The host has no volume
control to apply, so the stream is bit for bit what the synth sends. Full speed isochronous
endpoints have one transaction per 1 ms frame (bInterval 1 is the shortest). The 125 µs
microframes are a high speed feature, and the STM32H743's OTG1 port is full speed only.

Class requests (interface 3, entity 3), in `usb_audio_requests_hook`:

| Request | Reply |
|---|---|
| GET CUR sampling frequency (wLength 4) | 48000 |
| GET RANGE sampling frequency (wLength 2 or 14) | 1 subrange: 48000, 48000, 0 |
| GET CUR clock validity (wLength 1) | 1 |
| SET CUR sampling frequency (wLength 4) | accepted and ignored (only 48 kHz exists) |
| anything else | stall |

## Streaming

The synth thread writes every 32-frame block into a 512-frame ring, streaming or not.
The USB ISR builds one packet per 1 ms frame. A servo on the low-passed ring level gives the
47/48/49-frame sequence that follows the codec clock, so the stream never drifts
(`usb_audio_core.h`).

- **Ring target** (the level kept before each packet): 96 frames = 2 ms by default, was 144.
  It is set live with `Stct<3 digits>` (49..256 frames) and is not saved.
- **Fast start.** The ring always holds the latest synth output. A stream starts one target
  (plus one block of headroom) behind the writer, so the first packet is already audio.
  The old firmware sent 3 ms of priming silence. Priming remains for an underrun.
- **Start headroom.** A (re)centred stream starts 32 frames above the target. The level before
  a packet ripples by about ±30 frames with the phase of the synth blocks against the USB
  frames. Starting exactly at the target on a ripple peak left the mean up to 30 frames low:
  the restart simulation underran without the headroom.
- **Missed frames.** When the host does not fetch a packet in its frame, the packet is dropped
  and the next one built fresh: the stream stays in real time. The old firmware re-sent it,
  which added 1 ms of latency per miss and made the servo swing the rate to drain it.
  If the LLD flushed a packet due in the *next* frame, it is sent again (nothing lost).
- **Lost synth blocks.** Under CPU overload the synth can miss a codec interrupt: the two
  wakeup events merge and a block is never rendered. The jack then repeats a stale buffer, and
  the ring gets 32 frames less, which at a 2 ms target is often an underrun (about 2.7 ms of
  silence). The writer detects the loss (codec interrupts minus commits, not counting a wakeup
  already pending) and commits the last block again, the jack's glitch and no more, for up to 2
  lost blocks. It does this at the next commit, so a few still underrun (the simulation: 3 of 8).
  A longer stall underruns as before.
- **EOPF at 95%.** The incomplete isochronous IN interrupt moves from 80% to 95% of the frame
  (DCFG.PFIVL). The host may schedule the transaction anywhere in the first 90% of the frame,
  and a 294 byte packet takes about 0.23 ms on the bus. At 80%, a transfer that starts after
  about 57% of the frame was flushed in flight. That is likely behind a hub or a dock, with
  other periodic traffic, and the LLD then disabled the endpoint.

Margin of each target in the simulation (`target_sweep` in `test_core.c`). The margin is the
lowest level seen before a packet minus the 49-frame packet; below 0 means underruns. Latency is
from the synth's commit to the bus:

| target | light synth (render 10..20% of a block, ISR jitter 50 µs) | any render time (5..95%), ISR jitter 250 µs |
|---|---|---|
| 72 (1.5 ms) | margin 0, no underruns, 2.0 ms avg / 2.7 ms max | underruns |
| 80 (1.67 ms) | margin 7, 2.2 / 3.0 ms | underruns |
| 88 (1.83 ms) | margin 16 | margin 4 |
| **96 (2 ms)** | margin 23, 2.5 / 3.4 ms | margin 12, 2.4 / 3.5 ms |
| 144 (3 ms, old) | margin 72, 3.5 / 4.4 ms | margin 60, 3.4 / 4.5 ms |

The floor is structural. The level must cover the packet (49 frames), the 32-frame steps in
which the synth adds frames, and the synth's render time, which can move a commit by most of a
block. Below 2 ms, the margin depends on how heavy the current voice is.

## Latency budget (device side, from a synth sample to the bus)

| Stage | Time |
|---|---|
| Block quantisation and render: a control change reaches the next block (up to 0.67 ms) and the block is ready after its render time (up to ~0.6 ms). The jack has this too. | 0.1 .. 1.3 ms |
| Ring: commit to packet build, at target 96 | ~1.5 ms avg (0.4 .. 2.6) |
| Packet armed to fetch: a packet built after one fetch goes out at the next | 1 ms |
| On the bus (294 bytes at 12 Mbit/s) | ~0.23 ms |
| **Commit to bus, simulated** | **2.5 ms avg, 3.6 ms max** (was 3.5 / 4.6 at target 144) |

The host adds its own share: the USB driver's buffering, the audio engine period, and the
output device's buffer.

## Diagnostics: `StcU`

`tools/striso.py cmd U` prints the state and the counters since the last `U`, then resets them.
Run it once to reset, play or record, and run it again.

```
usb audio (UAC2): streaming 48000 Hz 24 bit (alt 1), source synth
since last U: packets sent 30012 missed 0 (host skipped 0, flushed early 0); built 30012 (47:31 48:29950 49:31)
rate: 1440560 frames in 30012 ms of host clock = 48000.266 Hz
ring: level 104 now, 96 filtered, 65..128 before packets (target 96 = 2000 us, of 512)
faults: underruns 0 overruns 0 primes 0 (silent packets 0), fast starts 1
frames: not ready at SOF 0; SOF late (>1.1 ms) 0 lost 0, max interval 1012 us
builds: after fetch 30012, after miss 0, at SOF 1; fetch at 95..140 us (avg 110) after SOF; build avg 1900 max 2600 cycles
synth: commit 210 us avg, 420 max after its DMA interrupt (block 667 us); off cadence 0; since boot: blocks lost 0, repeated 0
cpu: usb audio ISR hooks 9000000 cycles = 0.22% over 30012 ms; longest lock: thread 12 us, ISR 4 us
```

(The numbers above are illustrative of a healthy stream, not a measurement.)

| Line | Good | What else means |
|---|---|---|
| packets sent / missed | missed 0 (a few at a stream start are normal) | **host skipped** > 0: the packet was in the FIFO and the host did not fetch it, a host-side gap (driver, DPC latency). **flushed early** > 0: the LLD flushed a packet due in the next frame (sent again, nothing lost); if it is about one per frame, the EONUM parity check is not doing what it should. |
| rate | 48000 ± 0.1 Hz, steady between reads | It is measured in the host's frame clock, so a constant offset is the two crystals. If it wanders, the servo is moving. |
| ring | lowest level before packets ≥ ~55, no "priming" | A lowest level near 49 is close to underrunning: raise the target. |
| faults | all 0; fast starts = the number of stream starts | Underruns are device-side dropouts (each costs about the target in silence). Overruns: the host stopped fetching for a long time. |
| frames | not ready 0, SOF late 0, lost 0, max interval < 1100 µs | **not ready**: a packet was not in the FIFO at its frame's SOF (the device was late; the host got nothing that frame). **SOF late / lost**: the USB interrupt was held off (> 100 µs / > 1 ms) by lock sections or higher priority interrupts. |
| builds | "after miss" 0; fetch window well below 950 µs | The fetch window is the host's schedule. Above ~800 µs the old firmware (EOPF at 80%) would have flushed packets in flight. |
| synth | commit max < block (667 µs), off cadence 0, blocks lost 0 | **Off cadence**: a render ran past the next DMA interrupt (CPU overload: the jack glitched too). **Blocks lost** (since boot): blocks never rendered (the jack repeated a buffer); each is repeated on USB, and some still underrun at a low target. Fix the voice's CPU, or raise the target to ~120 to absorb them. |
| cpu, longest lock | ISR hooks < 1%, locks < 100 µs | A lock section holds off every interrupt at priority ≥ 2, the USB one included. Up to ~0.5 ms is harmless (a packet is armed ~1 ms ahead), more is a risk. Needs `STRISO_KERNEL_STATS` (on by default). |

Other commands: `Stcu` toggles the test tone on USB (left 1 kHz, right 500 Hz, -6 dBFS; check
a recording with `tools/usbaudio/analyze_tone.py rec.wav`). `Stct<nnn>` sets the ring target in
frames, and `Stct000` prints it.

## Latency marker: `Stcl1`, `Stcl0`

For measuring the USB audio latency electronically (Striso Live's `packages/striso_bench`):

- `Stcl1` writes a marker over the first 32 frames of the next packet the USB ISR builds, on the
  right channel only (the left keeps the synth). The marker is a 32-chip pseudo-random sequence
  (`ua_mark_pn` in `usb_audio_core.h`: a 5-bit maximal-length LFSR, x^5 + x^3 + 1, plus one chip)
  at -20 dBFS (±0.1 of full scale, exact in 24 and 16 bit). Its aperiodic autocorrelation
  sidelobes are at most 5/32, so a host finds it by correlation in any audio. It goes into the
  packed packet only: the ring, and so the jack, never have it. Once the host fetched that packet,
  the console thread replies:
  `usb audio marker <seq>: frame <fn> index 0 level <ring> target <t>, bus <us> us after rx (rx frame <fn>), right 32 frames -20 dBFS <bits> bit`.
  Here `bus` is the time (DWT) from the command's OUT packet arriving to the marker's IN transfer
  completing, and `level` is the ring before that packet. One marker at a time: `busy` while one
  is on its way, `lost` when the host skipped its packet, `not streaming` after 100 ms without a
  stream.
- `Stcl0` replies `usb audio echo: held <us> us`: from the command's OUT packet to the SOF that
  flushes this reply. A host subtracts it from the round trip; what is left is the console's
  transit (both ways).

The bulk driver notes when every OUT packet arrives (`bdu_rx_cycles`, `bdu_rx_frame`).
`test_core.c` checks the marker: only the right channel of 32 frames changes, to exactly ±0.1,
in 24 and 16 bit; the ring is untouched; it is idempotent; the sidelobes are small.

Measured with it on the development PC (Windows 11, usbaudio2, exclusive capture at its 3 ms
minimum period): 2.5 ms median from the bus to the app's capture callback (3.5 p95); the board's
ring before the packet about 2.4 to 2.9 ms (reported). The UAC1 build measured the same in
exclusive mode, and worse in shared mode (480-frame buffers: 6.5 ms against UAC2's 3.5 ms).

## Tests

`tools/test.sh` runs `usb_audio_core`, the simulation of the exact firmware core:

- clock offsets of ±2000 ppm;
- fast start and boot priming;
- 998 random stream restarts;
- host frame skips, single and in bursts, and a 50 ms host pause;
- ISR jitter up to 0.5 ms;
- synth stalls and lost blocks;
- live target changes;
- the target sweep above.

`ctest --preset firmware` checks the descriptors of the built ELF (UAC2 or UAC1).
