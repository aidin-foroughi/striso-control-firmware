"""Analysis of fx_test renders (stereo interleaved float32 at 48828 Hz).

  fx_analyze.py ir X.f32 [--skip MS]        reverb impulse response: energy, RT60 (T30 from the Schroeder
                                            curve), envelope flutter, persistent spectral peaks (ringing)
  fx_analyze.py level X.f32 X_in.f32        level vs input: peak, RMS, loudness per 0.5 s, sample jumps
  fx_analyze.py tone X.f32 FREQ [--from S]  spectral purity of a sine input: energy away from the carrier
  fx_analyze.py decay X.f32 T_END           RMS per second after the input stopped at T_END (stability)
"""
import sys
import numpy as np

SR = 48828


def load(p):
    x = np.fromfile(p, dtype=np.float32).astype(np.float64)
    return x.reshape(-1, 2)


def db(x):
    return 10 * np.log10(np.maximum(x, 1e-300))


def ir(path, skip_ms=1.0):
    x = load(path)
    s = int(skip_ms * 1e-3 * SR)
    x = x[int(0.05 * SR):]  # the impulse is at 50 ms
    x[:s] = 0  # remove the dry impulse (it comes out after the 16 sample limiter latency)
    for c, name in enumerate("LR"):
        h = x[:, c]
        energy = np.sum(h ** 2)
        # Schroeder backward integration
        edc = np.cumsum(h[::-1] ** 2)[::-1]
        edc_db = db(edc / edc[0])
        def t_at(level):
            i = np.argmax(edc_db <= level)
            return i / SR if edc_db[i] <= level else float("nan")
        t5, t35 = t_at(-5), t_at(-35)
        rt = (t35 - t5) * 2 if np.isfinite(t35) else float("nan")
        edt = t_at(-10) * 6
        # flutter: 5 ms RMS envelope (dB) minus a smooth trend (100 ms moving average) between -5 and -45 dB
        w = int(0.005 * SR)
        n = len(h) // w
        env = db(np.mean(h[:n * w].reshape(n, w) ** 2, axis=1))
        i0 = int(t5 * SR / w) + 1
        i1 = int(t_at(-45) * SR / w) if np.isfinite(t_at(-45)) else n
        seg = env[i0:i1]
        k = 20
        trend = np.convolve(seg, np.ones(k) / k, mode="same")
        resid = (seg - trend)[k:-k]
        # periodicity of the residual (discrete echoes / flutter would show as a peak)
        r = resid - resid.mean()
        ac = np.correlate(r, r, mode="full")[len(r) - 1:]
        ac = ac / ac[0]
        acmax = np.max(ac[2:min(len(ac), 40)]) if len(ac) > 3 else float("nan")
        # ringing: average of energy-normalized spectra over the late tail (-20..-50 dB); a persistent
        # mode stands out of the 1/3-octave smoothed average spectrum. The same statistic for ideal
        # (Gaussian noise) frames is computed for reference.
        F = 4096
        a0 = int(t_at(-20) * SR)
        a1 = int(t_at(-50) * SR) if np.isfinite(t_at(-50)) else len(h)
        freqs = np.fft.rfftfreq(F, 1 / SR)
        band = (freqs > 150) & (freqs < 5000)
        win = np.hanning(F)
        starts = list(range(a0, max(a0 + 1, a1 - F), F // 2))
        def peak_prom(frames):
            spec = np.zeros(F // 2 + 1)
            for fr in frames:
                S = np.abs(np.fft.rfft(fr * win)) ** 2
                spec += S / S.sum()
            sm = np.zeros_like(spec)
            for i in np.nonzero(band)[0]:
                f = freqs[i]
                m = (freqs >= f / 2 ** (1 / 6)) & (freqs <= f * 2 ** (1 / 6))
                sm[i] = np.mean(spec[m])
            prom = db(spec[band] / sm[band])
            return prom
        prom = peak_prom([h[i:i + F] for i in starts])
        rng = np.random.default_rng(7)
        ref = np.median([peak_prom([rng.standard_normal(F) for _ in starts]).max() for _ in range(5)])
        top = np.argsort(prom)[-3:][::-1]
        cnt = len(starts)
        print(f"{name}: IR energy {energy:.3f} ({db(energy):+.1f} dB), RT60 (T30) {rt:.2f} s, EDT {edt:.2f} s, "
              f"envelope residual std {np.std(resid):.2f} dB, residual autocorr max {acmax:.2f}, "
              f"late spectrum peaks (150-5000 Hz, {cnt} frames): " +
              ", ".join(f"{freqs[band][i]:.0f} Hz +{prom[i]:.1f} dB" for i in top) + f" (ideal noise: +{ref:.1f} dB)")
    corr = np.sum(x[:, 0] * x[:, 1]) / np.sqrt(np.sum(x[:, 0] ** 2) * np.sum(x[:, 1] ** 2))
    print(f"L/R correlation of the IR {corr:+.3f}")


def level(path, in_path):
    x = load(path)
    d = np.fromfile(in_path, dtype=np.float32).astype(np.float64)
    n = min(len(d), len(x))
    x, d = x[:n], d[:n]
    w = int(0.5 * SR)
    m = n // w
    ld = db(np.mean(d[:m * w].reshape(m, w) ** 2, axis=1))
    lw = db(np.mean(x[:m * w].reshape(m, w, 2) ** 2, axis=(1, 2)))
    loud = ld > ld.max() - 30
    diff = lw[loud] - ld[loud]
    print(f"peak in {np.abs(d).max():.3f} out {np.abs(x).max():.3f} | RMS in {np.sqrt(np.mean(d**2)):.3f} "
          f"out L {np.sqrt(np.mean(x[:,0]**2)):.3f} R {np.sqrt(np.mean(x[:,1]**2)):.3f} | "
          f"loudness out-in per 0.5 s: median {np.median(diff):+.2f} dB, min {diff.min():+.2f}, max {diff.max():+.2f}")
    # sample to sample jumps (second difference) relative to the input's
    dd = np.abs(np.diff(d, 2)).max()
    dx = np.abs(np.diff(x, 2, axis=0)).max()
    print(f"max |2nd difference| in {dd:.3f} out {dx:.3f}")


def tone(path, f0, t_from=0.5):
    x = load(path)
    x = x[int(t_from * SR):]
    F = 1 << int(np.log2(len(x)))
    F = min(F, 1 << 18)
    win = np.blackman(F)
    freqs = np.fft.rfftfreq(F, 1 / SR)
    for c, name in enumerate("LR"):
        S = np.abs(np.fft.rfft(x[:F, c] * win)) ** 2
        near = np.abs(freqs - f0) < 200
        far = (~near) & (freqs > 20)
        print(f"{name}: energy beyond +-200 Hz of {f0:.0f} Hz: {db(S[far].sum() / S.sum()):.1f} dB, "
              f"strongest far component {freqs[far][np.argmax(S[far])]:.0f} Hz at {db(S[far].max() / S[near].max()):.1f} dB "
              f"re carrier")


def decay(path, t_end):
    x = load(path)
    i = int(t_end * SR)
    rms = []
    for k in range(i, len(x) - SR + 1, SR):
        rms.append(np.sqrt(np.mean(x[k:k + SR] ** 2)))
    print("RMS per second after the input stopped: " + " ".join(f"{20*np.log10(max(r,1e-30)):.0f}" for r in rms) + " dB")
    grow = [b > a * 1.05 for a, b in zip(rms, rms[1:]) if a > 1e-6]
    print("monotonic decay" if not any(grow) else "!! LEVEL GROWS after the input stopped")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "ir":
        ir(a[1], float(a[3]) if len(a) > 3 and a[2] == "--skip" else 1.0)
    elif a[0] == "level":
        level(a[1], a[2])
    elif a[0] == "tone":
        tone(a[1], float(a[2]), float(a[4]) if len(a) > 4 else 0.5)
    elif a[0] == "decay":
        decay(a[1], float(a[2]))
