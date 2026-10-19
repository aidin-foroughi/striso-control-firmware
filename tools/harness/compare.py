"""Compare two rendered test performances. Usage: compare.py ref.f32 test.f32 [--wav] [--perceptual]

--perceptual compares loudness envelopes (20 ms) and 1/3-octave band energies (84 ms frames)
instead of samples, for changes that alter the noise realization but should not alter the sound."""
import sys
import wave
import numpy as np

SR, SEG = 48828, 0.5

def load(p):
    return np.fromfile(p, dtype=np.float32).astype(np.float64)

def db(x):
    return 10 * np.log10(x) if x > 0 else float("inf")

def perceptual(ref, test):
    w = int(0.02 * SR)
    n = len(ref) // w
    er = np.sqrt(np.mean(ref[:n * w].reshape(n, w) ** 2, axis=1))
    et = np.sqrt(np.mean(test[:n * w].reshape(n, w) ** 2, axis=1))
    loud = er > er.max() * 10 ** (-40 / 20)
    d = 20 * np.log10((et[loud] + 1e-12) / er[loud])
    print(f"loudness envelope (20 ms, frames within 40 dB of peak): median |diff| {np.median(np.abs(d)):.2f} dB, "
          f"95% {np.percentile(np.abs(d), 95):.2f} dB, max {np.abs(d).max():.2f} dB")
    F = 4096
    win = np.hanning(F)
    freqs = np.fft.rfftfreq(F, 1 / SR)
    edges = 100 * 2 ** (np.arange(0, 22) / 3)  # 100 Hz .. 16 kHz
    br, bt = [], []
    for i in range(0, len(ref) - F, F):
        R = np.abs(np.fft.rfft(ref[i:i + F] * win)) ** 2
        T = np.abs(np.fft.rfft(test[i:i + F] * win)) ** 2
        br.append([R[(freqs >= a) & (freqs < b)].sum() for a, b in zip(edges[:-1], edges[1:])])
        bt.append([T[(freqs >= a) & (freqs < b)].sum() for a, b in zip(edges[:-1], edges[1:])])
    br, bt = np.array(br), np.array(bt)
    m = br > br.max() * 1e-6
    d = 10 * np.log10((bt[m] + 1e-30) / br[m])
    print(f"band energies (1/3 oct, within 60 dB of peak): median |diff| {np.median(np.abs(d)):.2f} dB, "
          f"95% {np.percentile(np.abs(d), 95):.2f} dB, mean diff {np.mean(d):+.2f} dB")

def main(ref_p, test_p, wav=False, percep=False):
    ref, test = load(ref_p), load(test_p)
    n = min(len(ref), len(test))
    ref, test = ref[:n], test[:n]
    err = test - ref
    print(f"peak ref {np.abs(ref).max():.4f}  peak test {np.abs(test).max():.4f}  max|err| {np.abs(err).max():.3g}")
    print(f"overall SNR {db(np.sum(ref**2) / np.sum(err**2)):.1f} dB" if np.any(err) else "bit-identical")
    if np.any(err):
        # worst segment, ignoring near-silent ones
        seg = int(SEG * SR)
        worst = (float("inf"), 0)
        for i in range(0, n - seg, seg):
            p = np.sum(ref[i:i + seg] ** 2)
            if p > seg * 1e-8:
                worst = min(worst, (db(p / max(np.sum(err[i:i + seg] ** 2), 1e-300)), i / SR))
        print(f"worst {SEG}s segment SNR {worst[0]:.1f} dB at t={worst[1]:.1f}s")
    if percep:
        perceptual(ref, test)
    if not np.all(np.isfinite(test)):
        print("!! NON-FINITE SAMPLES IN TEST OUTPUT")
    if wav:
        # scaled like the board at its usual volume: (63/127)^2 ~ 0.25, then clipped at +-1
        for p, x in ((ref_p, ref), (test_p, test)):
            with wave.open(p.replace(".f32", ".wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
                w.writeframes((np.clip(0.25 * x, -1, 1) * 32767).astype("<i2").tobytes())

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], "--wav" in sys.argv, "--perceptual" in sys.argv)
