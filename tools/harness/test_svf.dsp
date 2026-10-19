import("stdfaust.lib");
// Same filter as fi.svf.lp (T=0) and fi.svf.bp (T=1), Andrew Simper's trapezoidal SVF, but
// - tan() is a [7/6] Pade approximant, within 2 float ulps of tanf for cutoffs up to 16 kHz
// - the per-sample divisions (1/Q, two of 1/(1+g(g+k)), and g) are folded into one
// With g = gN/D and k = 1/Q, the update v1 = (ic1 + g(v0-ic2)) / (1+g(g+k)), v2 = ic2 + g*v1
// becomes v1 = a*ic1 + ga*(v0-ic2), v2 = ic2 + ga*ic1 + g2a*(v0-ic2) with one reciprocal r.
svf_fast(T, F, Q) = tick ~ (_,_) : !,!,out
with {
    x = F * ma.PI / ma.SR;
    x2 = x * x;
    gN = x * (135135 + x2 * (-17325 + x2 * (378 - x2)));
    D = 135135 + x2 * (-62370 + x2 * (3150 - 28 * x2));
    QD = Q * D;
    r = 1 / (QD * D + gN * (gN * Q + D));
    a = QD * D * r;
    ga = gN * QD * r;
    g2a = gN * gN * Q * r;
    tick(ic1eq, ic2eq, v0) = 2*v1 - ic1eq, 2*v2 - ic2eq, v1, v2
    with {
        v1 = a * ic1eq + ga * (v0 - ic2eq);
        v2 = ic2eq + ga * ic1eq + g2a * (v0 - ic2eq);
    };
    out = case {
        (0) => !,_;
        (1) => _,!;
    } (T);
};
svf_lp(F, Q) = svf_fast(0, F, Q);
svf_bp(F, Q) = svf_fast(1, F, Q);
// inputs: signal, lp cutoff (Hz), lp Q; bp uses cutoff*0.2 with Q 2
process(x, F, Q) = x <: fi.svf.lp(F, Q), svf_lp(F, Q), fi.svf.bp(F*0.2, 2), svf_bp(F*0.2, 2);
