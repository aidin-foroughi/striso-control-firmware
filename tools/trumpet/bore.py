"""Transmission-line (TMM) model of trumpet-family bores with visco-thermal losses and an unflanged
radiation load (Silva et al. 2009), used to design and fit the real-time waveguide of voice_trumpet.h.

Profile: list of (length, r_in, r_out) conical pieces from the throat (just after the cup) to the bell
mouth. The cup is a lumped compliance (volume V) at the input.
"""
import numpy as np

RHO, C0, ETA = 1.2, 343.0, 1.85e-5


def cone_tm(L, r1, r2, f):
    """transfer matrix of a lossy conical frustum (approximated by n stepped cylinders)"""
    n = max(1, int(np.ceil(L / 0.002)))
    xs = (np.arange(n) + 0.5) / n
    rs = r1 + (r2 - r1) * xs
    w = 2 * np.pi * f
    A = np.ones_like(f, complex); B = np.zeros_like(f, complex); Cc = np.zeros_like(f, complex); D = np.ones_like(f, complex)
    dl = L / n
    for r in rs:
        rv = r * np.sqrt(w * RHO / ETA)
        k = w / C0 * (1 + 1.045 / rv) - 1j * w / C0 * 1.045 / rv
        S = np.pi * r * r
        Zc = RHO * C0 / S * (1 + 0.369 / rv - 1j * 0.369 / rv)   # small lossy correction
        ch, sh = np.cos(k * dl), 1j * np.sin(k * dl)
        a, b, c, d = ch, Zc * sh, sh / Zc, ch
        A, B, Cc, D = A * a + B * c, A * b + B * d, Cc * a + D * c, Cc * b + D * d
    return A, B, Cc, D


def chain(profile, f):
    A = np.ones_like(f, complex); B = np.zeros_like(f, complex); Cc = np.zeros_like(f, complex); D = np.ones_like(f, complex)
    for (L, r1, r2) in profile:
        a, b, c, d = cone_tm(L, r1, r2, f)
        A, B, Cc, D = A * a + B * c, A * b + B * d, Cc * a + D * c, Cc * b + D * d
    return A, B, Cc, D


def zrad(r, f):
    """unflanged pipe radiation impedance (Silva, Guillemain, Kergomard, Mallaroni, Norris 2009)"""
    ka = 2 * np.pi * f / C0 * r
    Rm = (1 + 0.2 * ka - 0.084 * ka ** 2) / (1 + 0.2 * ka + (0.5 - 0.084) * ka ** 2)
    dl = r * 0.6133 * (1 + 0.044 * ka ** 2) / (1 + 0.19 * ka ** 2) - r * 0.02 * np.sin(2.5 * ka) ** 2
    Rr = -Rm * np.exp(-2j * ka * dl / r)
    Zc = RHO * C0 / (np.pi * r * r)
    return Zc * (1 + Rr) / (1 - Rr)


def bessel_bell(L, r0, r1, gamma, n=40):
    """r(s) = B (s + s0)^-gamma, s = distance from the mouth; r(L) = r0 (throat), r(0) = r1 (mouth)"""
    # solve s0: r1/r0 = ((L + s0)/s0)^gamma
    q = (r1 / r0) ** (1 / gamma)
    s0 = L / (q - 1)
    B = r1 * s0 ** gamma
    s = np.linspace(L, 0, n + 1)
    # finer steps near the mouth
    t = np.linspace(0, 1, n + 1) ** 0.6
    s = L * (1 - t)
    r = B * (s + s0) ** -gamma
    return [(s[i] - s[i + 1], r[i], r[i + 1]) for i in range(n)]


def trumpet_profile(Lc=0.55, Lb=0.53, gamma=0.75, r_bore=5.8e-3, r_mouth=61.5e-3, extra=0.0,
                    backbore=(0.06, 1.83e-3, 4.3e-3), leadpipe=(0.24, 4.4e-3, 5.8e-3), throat=(0.007, 1.83e-3)):
    p = [(throat[0], throat[1], throat[1]), backbore, leadpipe, (Lc + extra, r_bore, r_bore)]
    return p + bessel_bell(Lb, r_bore, r_mouth, gamma)


def zin(profile, f, V=0.75e-6):
    A, B, Cc, D = chain(profile, f)
    zr = zrad(profile[-1][2], f)
    zt = (A * zr + B) / (Cc * zr + D)
    w = 2 * np.pi * f
    Ccup = V / (RHO * C0 ** 2)
    z = 1 / (1j * w * Ccup + 1 / zt)
    return z, zt, (A, B, Cc, D, zr)


def peaks(f, z, fmin=50, fmax=1600):
    a = np.abs(z)
    i = np.nonzero((a[1:-1] > a[:-2]) & (a[1:-1] > a[2:]) & (f[1:-1] > fmin) & (f[1:-1] < fmax))[0] + 1
    out = []
    for k in i:
        # parabolic interpolation in log
        y0, y1, y2 = np.log(a[k - 1]), np.log(a[k]), np.log(a[k + 1])
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
        out.append((f[k] + d * (f[1] - f[0]), a[k]))
    return out


if __name__ == '__main__':
    f = np.arange(20, 3000, 0.5)
    prof = trumpet_profile()
    z, zt, _ = zin(prof, f)
    tot = sum(p[0] for p in prof)
    print('length', tot)
    f0 = 466.16 / 4
    for (fp, a) in peaks(f, z):
        n = round(fp / f0)
        print(f'{fp:8.1f} Hz  n={n:2d}  {1200 * np.log2(fp / (n * f0)) if n else 0:7.1f} c  |Z| {20 * np.log10(a / 1e6):5.1f} dB re 1 MOhm')
