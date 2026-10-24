"""Design the real-time bore of voice_trumpet.h from the TMM physics (bore.py).

Real-time model at FS = 96 kHz (2x the board rate):
  lips -> cup compliance -> front end: a Kelly-Lochbaum ladder of short cylinders (throat, backbore,
  leadpipe; integer sample delays) -> cylinder: forward/backward delay lines (valves and the tuning
  slide change their length; the forward line steepens) -> bell: reflection = decimate by 8 -> FIR at
  12 kHz -> interpolate by 8 (exact multirate convolution with the bell's reflection function).
The bell FIR is fitted (linear least squares) so that the model's input impedance equals the TMM
input impedance of the real bore, losses included (de-embedding through the lossless front end).
The radiation transfer (bell entry forward wave -> far field) is fitted as a minimum phase IIR at 48 kHz.
Writes voice_trumpet_bore.h (tables) and prints the impedance peaks of TMM vs model.
"""
import sys
import numpy as np
import bore

FS = 96000
DEC = 8
FS12 = FS // DEC
NDEC = 48          # decimation / interpolation FIR taps (at 96 kHz)
LAT = (NDEC - 1) / 2
NFIR = 128  # their delay each (samples at 96 kHz)


def lowpass_fir(n, fc, fs):
    t = np.arange(n) - (n - 1) / 2
    h = np.sinc(2 * fc / fs * t) * np.kaiser(n, 6.0)
    return h / h.sum()


HDEC = lowpass_fir(NDEC, 4200, FS)          # decimator (gain 1)
HINT = HDEC * DEC                            # interpolator (gain DEC, zero stuffed input)


def front_sections(prof_front, fs=FS):
    """stepped cylinders with integer sample lengths from the conical front profile"""
    secs = []
    for (L, r1, r2) in prof_front:
        nsamp = L / bore.C0 * fs
        if r1 == r2:
            n = max(1, int(round(nsamp)))
            secs.append((n, r1))
            continue
        piece = 6 if r2 / r1 > 1.6 else 22   # backbore: ~2 cm pieces, leadpipe ~8 cm
        k = max(1, int(round(nsamp / piece)))
        ns = [int(round(nsamp * (i + 1) / k)) - int(round(nsamp * i / k)) for i in range(k)]
        for i, n in enumerate(ns):
            x = (i + 0.5) / k
            r = r1 + (r2 - r1) * x
            secs.append((max(1, n), r))
    return secs


def chain_lossless(secs, f, fs=FS):
    w = 2 * np.pi * f
    A = np.ones_like(f, complex); B = np.zeros_like(f, complex); C = np.zeros_like(f, complex); D = np.ones_like(f, complex)
    for (n, r) in secs:
        Z = bore.RHO * bore.C0 / (np.pi * r * r)
        th = w * n / fs
        a, b, c, d = np.cos(th), 1j * Z * np.sin(th), 1j * np.sin(th) / Z, np.cos(th)
        A, B, C, D = A * a + B * c, A * b + B * d, C * a + D * c, C * b + D * d
    return A, B, C, D


class Inst:
    def __init__(self, name, front, Lc, bell, r_bore, V, f_nom, cyl_loss=1.0):
        self.name, self.front, self.Lc, self.bell, self.r_bore, self.V, self.f_nom = name, front, Lc, bell, r_bore, V, f_nom

    def profile(self, extra=0.0):
        return self.front + [(self.Lc + extra, self.r_bore, self.r_bore)] + self.bell


def make_insts():
    out = []
    rb = 5.83e-3
    fr = [(0.007, 1.83e-3, 1.83e-3), (0.06, 1.83e-3, 4.3e-3), (0.24, 4.4e-3, rb)]
    out.append(Inst('bb', fr, 0.564, bore.bessel_bell(0.53, rb, 61.5e-3, 0.75), rb, 0.80e-6, 466.16 / 4))
    # lead: tighter backbore, shallow cup (V set per preset in the engine; the bore is the same)
    # C trumpet: shorter cylinder and bell, same mouthpiece
    out.append(Inst('c', fr, 0.564 - 0.168, bore.bessel_bell(0.50, rb, 60e-3, 0.75), rb, 0.75e-6, 523.25 / 4))
    # piccolo in Bb (an octave above): small bore, small bell, shallow cup
    rp = 5.0e-3
    frp = [(0.006, 1.75e-3, 1.75e-3), (0.045, 1.75e-3, 3.8e-3), (0.13, 3.9e-3, rp)]
    out.append(Inst('picc', frp, 0.205, bore.bessel_bell(0.30, rp, 50e-3, 0.7), rp, 0.45e-6, 932.33 / 4))
    # cornet: deep V cup, long conical backbore/leadpipe, longer conical bell (tune_profiles.py)
    frc = [(0.008, 1.95e-3, 1.95e-3), (0.075, 1.95e-3, 4.2e-3), (0.25685, 4.3e-3, rb)]
    out.append(Inst('cornet', frc, 0.3555, bore.bessel_bell(0.67061, rb, 60e-3, 0.56922), rb, 1.05e-6, 466.16 / 4))
    return out


def tune_length(inst, target_n=4):
    """adjust the cylinder so resonance target_n is in tune with the nominal series"""
    f = np.arange(30, 2000, 0.25)
    for it in range(6):
        z, zt, _ = bore.zin(inst.profile(), f, inst.V)
        pk = [p for p, a in bore.peaks(f, z) if abs(p / inst.f_nom - target_n) < 0.4]
        if not pk:
            break
        err = pk[0] / (target_n * inst.f_nom)
        # length change: the resonance scales ~ with the total acoustic length (~ c/(2 f_nom))
        Leff = bore.C0 / (2 * inst.f_nom)
        inst.Lc += Leff * (err - 1)
    return inst


def fit_inst(inst, nfir=NFIR, verbose=True):
    N = 1 << 14
    f = np.fft.rfftfreq(N, 1 / FS)
    f[0] = 0.5
    w = 2 * np.pi * f
    prof = inst.profile()
    z, zt, _ = bore.zin(prof, f, inst.V)
    secs = front_sections(inst.front)
    A, B, C, D = chain_lossless(secs, f)
    ZL = (B - D * zt) / (C * zt - A)
    Zw = bore.RHO * bore.C0 / (np.pi * inst.r_bore ** 2)
    RL = (ZL - Zw) / (ZL + Zw)
    # cylinder one way delay realised as pure delay in the model (minus the multirate latency)
    dcyl = inst.Lc / bore.C0 * FS
    dpure = dcyl - LAT          # per direction: the decimator latency goes on the way in, the interpolator's on the way out
    # target for the multirate bell: Hdec * H12 * Hint = RL * exp(+2 j w dpure / FS)
    tgt = RL * np.exp(2j * w * dpure / FS)
    # composite response of decim/interp
    Hd = np.fft.rfft(HDEC, N)[: len(f)]          # with their latency (LAT each): dpure leaves room for it
    Hi = np.fft.rfft(HINT, N)[: len(f)] / DEC
    # linear least squares for h (12 kHz FIR): sum_k h_k exp(-j w k DEC / FS) * Hd * Hi = tgt
    band = f < FS12 / 2
    wt = np.where(f < 2500, 1.0 / (1.04 - np.minimum(np.abs(RL), 0.99)), np.where(f < 3600, 1.0, 3.0))
    tgt = np.where(f < 3600, tgt, 0.0)
    E = np.exp(-1j * np.outer(w[band], np.arange(nfir) * DEC / FS)) * (Hd * Hi)[band, None]
    Wb = wt[band]
    Ar = np.vstack([np.real(E) * Wb[:, None], np.imag(E) * Wb[:, None]])
    br = np.concatenate([np.real(tgt[band]) * Wb, np.imag(tgt[band]) * Wb])
    lam = 1e-4
    h = np.linalg.solve(Ar.T @ Ar + lam * np.eye(nfir), Ar.T @ br)
    # model response
    Hm = (np.exp(-1j * np.outer(w, np.arange(nfir) * DEC / FS)) @ h) * Hd * Hi
    Rm = Hm * np.exp(-2j * w * dpure / FS)
    ZLm = Zw * (1 + Rm) / (1 - Rm)
    ztm = (A * ZLm + B) / (C * ZLm + D)
    Ccup = inst.V / (bore.RHO * bore.C0 ** 2)
    zm = 1 / (1j * w * Ccup + 1 / ztm)
    if verbose:
        print(f'== {inst.name}: Lc {inst.Lc:.4f} m, total {sum(p[0] for p in prof):.3f} m, front sections {secs}')
        print(f'   |RL| max {np.abs(RL[f < 4000]).max():.3f} (passive <= 1), cylinder {dcyl:.1f} samples, fir max |h| {np.abs(h).max():.3f}, sum {h.sum():.3f}, max |Rm| {np.abs(Rm).max():.3f}')
        pt = bore.peaks(f, z, 40, 2000)
        pm = bore.peaks(f, zm, 40, 2000)
        for (a, b) in zip(pt, pm):
            n = round(a[0] / inst.f_nom)
            print(f'   n={n:2d} tmm {a[0]:7.1f} Hz {1200*np.log2(a[0]/(max(n,1)*inst.f_nom)):6.1f} c {20*np.log10(a[1]/1e6):5.1f} dB | model {b[0]:7.1f} Hz ({1200*np.log2(b[0]/a[0]):5.1f} c) {20*np.log10(b[1]/1e6):5.1f} dB')
    return dict(h=h, secs=secs, Zw=Zw, dcyl=dcyl, f=f, z=z, zm=zm, dpure=dpure, HdHi=Hd * Hi)


def radiation(inst):
    """far-field pressure per forward wave at the bell entry (relative), on a 48 kHz grid"""
    f = np.linspace(10, 24000, 4000)
    A, B, C, D = bore.chain(inst.bell, f)
    zr = bore.zrad(inst.bell[-1][2], f)
    Ze = (A * zr + B) / (C * zr + D)
    Zw = bore.RHO * bore.C0 / (np.pi * inst.r_bore ** 2)
    Rb = (Ze - Zw) / (Ze + Zw)
    Um = (1 + Rb) / (A * zr + B)
    T = 1j * 2 * np.pi * f * Um
    return f, T


def fit_minphase_iir(f, mag, fs, nsec=3, fmin=40, fmax=20000):
    """cascade of biquads (poles/zeros free) fitted to |mag| on a log scale"""
    from scipy.optimize import least_squares
    sel = (f > fmin) & (f < fmax)
    ff = f[sel]
    tgt = 20 * np.log10(np.abs(mag[sel]))
    zz = np.exp(-1j * 2 * np.pi * ff / fs)

    def resp(p):
        g = p[0]
        H = np.ones_like(zz) * 10 ** (g / 20)
        for s in range(nsec):
            rz, az, rp, ap = p[1 + 4 * s: 5 + 4 * s]
            rz = np.tanh(rz); rp = 0.999 * np.tanh(rp)
            zc = rz * np.exp(1j * az); pc = rp * np.exp(1j * ap)
            H = H * (1 - 2 * zc.real * zz + abs(zc) ** 2 * zz ** 2) / (1 - 2 * pc.real * zz + abs(pc) ** 2 * zz ** 2)
        return H

    def err(p):
        return 20 * np.log10(np.abs(resp(p)) + 1e-12) - tgt

    best = None
    rng = np.random.default_rng(1)
    for trial in range(12):
        p0 = [0.0]
        for s in range(nsec):
            p0 += [rng.uniform(0.3, 2.5), rng.uniform(0, np.pi), rng.uniform(0.5, 2.5), rng.uniform(0, np.pi)]
        r = least_squares(err, p0, max_nfev=4000)
        if best is None or r.cost < best.cost:
            best = r
    p = best.x
    secs = []
    for s in range(nsec):
        rz, az, rp, ap = p[1 + 4 * s: 5 + 4 * s]
        rz = np.tanh(rz); rp = 0.999 * np.tanh(rp)
        secs.append((1.0, -2 * rz * np.cos(az), rz * rz, -2 * rp * np.cos(ap), rp * rp))
    rms = np.sqrt(np.mean(err(p) ** 2))
    return 10 ** (p[0] / 20), secs, rms


if __name__ == '__main__':
    insts = make_insts()
    for inst in insts:
        tune_length(inst)
        fit_inst(inst)
