"""Check a recording of the Striso USB audio test tone ('Stcu': left 1 kHz, right 500 Hz, -6 dBFS).

  analyze_tone.py recording.wav

Reads WAV (PCM 16/24/32 bit or float32, as Audacity exports). For each channel it reports the
level, the frequency (in the recording's own sample clock: exactly 1000 / 500 Hz unless the host
resampled), and every glitch: a pure sine satisfies x[n] - 2 cos(w) x[n-1] + x[n-2] = 0, so a
dropped, repeated or zeroed sample shows as a spike in that residual (a dropped sample at 1 kHz
gives ~1e-2 full scale; 24 bit quantisation noise is ~1e-7). Exit code 1 if glitches are found.
"""
import struct, sys
import numpy as np


def read_wav(path):
    d = open(path, "rb").read()
    assert d[:4] == b"RIFF" and d[8:12] == b"WAVE", "not a WAV file"
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, n = d[i:i + 4], struct.unpack_from("<I", d, i + 4)[0]
        body = d[i + 8:i + 8 + n]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
            if fmt[0] == 0xFFFE:   # WAVE_FORMAT_EXTENSIBLE: real format in the sub-format GUID
                fmt = (struct.unpack_from("<H", body, 24)[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        i += 8 + n + (n & 1)
    tag, ch, sr, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, "<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, "<i2") / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, "<i4") / 2147483648.0
    else:
        raise SystemExit(f"unsupported WAV format {tag}, {bits} bit")
    return sr, x[:len(x) // ch * ch].reshape(-1, ch)


def freq_estimate(x, sr, f0):
    """Frequency from the phase slope of the signal demodulated at f0 (robust, sub-ppm)."""
    t = np.arange(len(x)) / sr
    z = x * np.exp(-2j * np.pi * f0 * t)
    n = int(sr / f0) * 10                     # average over 10 periods
    k = len(z) // n
    zb = z[:k * n].reshape(k, n).mean(axis=1)
    ph = np.unwrap(np.angle(zb))
    tb = (np.arange(k) + 0.5) * n / sr
    slope = np.polyfit(tb, ph, 1)[0]
    return f0 + slope / (2 * np.pi), 2 * np.abs(zb).mean()


def main():
    sr, x = read_wav(sys.argv[1])
    print(f"{sys.argv[1]}: {x.shape[1]} channels, {sr} Hz, {len(x) / sr:.1f} s")
    if sr != 48000:
        print("  note: not 48000 Hz: the host resampled (set the project / device rate to 48000 Hz)")
    bad = 0
    for c, f0 in enumerate([1000.0, 500.0][:x.shape[1]]):
        s = x[:, c]
        on = np.flatnonzero(np.abs(s) > 0.05)
        if len(on) < sr:
            print(f"  channel {c}: no tone")
            bad += 1
            continue
        s = s[on[0] + 100:on[-1] - 100]   # skip the fade in / out of the recording
        f, amp = freq_estimate(s, sr, f0)
        w = 2 * np.pi * f / sr
        e = s[2:] - 2 * np.cos(w) * s[1:-1] + s[:-2]
        floor = np.median(np.abs(e))
        thr = max(1e-4, 50 * floor)
        hits = np.flatnonzero(np.abs(e) > thr)
        # group hits closer than 10 samples into one event
        events = [] if not len(hits) else [hits[0]] + [h for p, h in zip(hits, hits[1:]) if h - p > 10]
        print(f"  channel {c} ({'left' if c == 0 else 'right'}): {20 * np.log10(amp):.2f} dBFS (expect -6.02), "
              f"{f:.4f} Hz = {(f / f0 - 1) * 1e6:+.2f} ppm vs {f0:.0f} Hz, residual floor {floor:.1e}, "
              f"glitches {len(events)}")
        for h in events[:20]:
            t = (on[0] + 100 + h + 1) / sr
            print(f"    glitch at {t:9.4f} s ({int(t // 60)}:{t % 60:07.4f}), size {np.abs(e[h:h + 10]).max():.2e}")
        bad += len(events)
    print("result:", "CLEAN" if bad == 0 else f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
