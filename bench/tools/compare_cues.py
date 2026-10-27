"""Compare a run's motion-cue signal against a reference run.

Both inputs are cue CSVs written by chrono_rt_bench --cues. The runs may use
different step sizes: the reference is linearly interpolated onto the test
run's time stamps. Samples before --from (default 1 s, the settle period) are
ignored.

For each cue channel it reports the RMS error, the RMS error relative to the
reference signal's RMS, and the largest absolute error. An optional
first-order low-pass (--lowpass HZ) is applied to both signals first, as a
stand-in for the motion platform's own bandwidth.

Standard library only:  python compare_cues.py REF.csv TEST.csv [--lowpass 20] [--from 1.0]
"""

import argparse
import csv
import math
import sys

CHANNELS = ["fx", "fy", "fz", "wx", "wy", "wz", "ax", "ay", "az"]


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"{path}: no samples")
    t = [float(r["t"]) for r in rows]
    cols = {c: [float(r[c]) for r in rows] for c in CHANNELS}
    return t, cols


def lowpass(t, x, hz):
    if hz <= 0:
        return x
    y = [x[0]]
    for i in range(1, len(x)):
        dt = t[i] - t[i - 1]
        a = dt / (dt + 1.0 / (2 * math.pi * hz))
        y.append(y[-1] + a * (x[i] - y[-1]))
    return y


def interp(t_src, x_src, t_dst):
    out = []
    j = 0
    n = len(t_src)
    for t in t_dst:
        while j + 1 < n and t_src[j + 1] < t:
            j += 1
        if j + 1 >= n:
            out.append(x_src[-1])
            continue
        t0, t1 = t_src[j], t_src[j + 1]
        w = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
        out.append(x_src[j] + w * (x_src[j + 1] - x_src[j]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("test")
    ap.add_argument("--lowpass", type=float, default=0.0, help="first-order low-pass cutoff [Hz], 0 = off")
    ap.add_argument("--from", dest="t_from", type=float, default=1.0, help="ignore samples before this time [s]")
    args = ap.parse_args()

    t_ref, ref = load(args.reference)
    t_tst, tst = load(args.test)
    t_end = min(t_ref[-1], t_tst[-1])
    keep = [i for i, t in enumerate(t_tst) if args.t_from <= t <= t_end]
    if not keep:
        sys.exit("no overlapping samples after --from")

    print(f"reference {args.reference}\ntest      {args.test}")
    print(f"window {t_tst[keep[0]]:.3f}..{t_tst[keep[-1]]:.3f} s, {len(keep)} samples, lowpass {args.lowpass or 'off'}")
    print(f"{'channel':>8} {'rms_err':>12} {'rel_rms':>9} {'max_abs':>12}")
    for c in CHANNELS:
        r = interp(t_ref, lowpass(t_ref, ref[c], args.lowpass), t_tst)
        x = lowpass(t_tst, tst[c], args.lowpass)
        err = [x[i] - r[i] for i in keep]
        rms_err = math.sqrt(sum(e * e for e in err) / len(err))
        rms_ref = math.sqrt(sum(r[i] * r[i] for i in keep) / len(keep))
        rel = rms_err / rms_ref if rms_ref > 0 else float("nan")
        print(f"{c:>8} {rms_err:12.5g} {rel:9.3%} {max(abs(e) for e in err):12.5g}")


if __name__ == "__main__":
    main()
