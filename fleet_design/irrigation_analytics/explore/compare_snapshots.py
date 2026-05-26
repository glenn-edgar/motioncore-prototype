#!/usr/bin/env python3
"""
Day-over-day diff of two snapshot pulls.

Usage:
  ./compare_snapshots.py 2026-05-26 2026-05-27
  ./compare_snapshots.py 2026-05-26 2026-05-27 --valve satellite_2:13

Compares:
  - per-valve today's R (after offset + wire correction)
  - shift in 20-day window
  - shift in MK trend τ + p
  - which valves moved into/out of anomaly classification
"""
import json, math, statistics, sys, argparse
from pathlib import Path

V_SUPPLY = 15.5
DISCONNECTS = {"satellite_1:1", "satellite_1:28", "satellite_1:38",
               "satellite_1:40", "satellite_3:1", "satellite_4:6"}
WIRE_OFFSET_OHMS = {**{f"satellite_2:{p}": 19.0 for p in (13,14,15,16,17)},
                    **{f"satellite_3:{p}":  3.0 for p in (11,12,13,14,15,16,17,18)}}


def offset(valve_test):
    return statistics.median([valve_test[v][-1] for v in DISCONNECTS
                              if v in valve_test and valve_test[v]])


def corr_r(valve, i_amps, off):
    i_true = i_amps - off
    if i_true <= 1e-4: return None
    r = V_SUPPLY / i_true - WIRE_OFFSET_OHMS.get(valve, 0.0)
    if valve == "satellite_1:44": r *= 2.0
    return r


def mann_kendall_r(series_amps, valve, off):
    rs = []
    for i in series_amps:
        r = corr_r(valve, i, off)
        if r is not None and 5.0 < r < 500.0:
            rs.append(r)
    n = len(rs)
    if n < 5: return 0.0, 1.0
    s = sum((1 if rs[j]>rs[i] else -1 if rs[j]<rs[i] else 0)
            for i in range(n-1) for j in range(i+1, n))
    var = n*(n-1)*(2*n+5)/18.0
    z = (s-1)/math.sqrt(var) if s>0 else ((s+1)/math.sqrt(var) if s<0 else 0)
    tau = s / (n*(n-1)/2.0)
    p = math.erfc(abs(z)/math.sqrt(2))
    return tau, p


def load_snapshot(date):
    p = Path(__file__).parent / "snapshots" / date / "valve_test.json"
    if not p.exists():
        raise SystemExit(f"snapshot missing: {p}")
    return json.loads(p.read_text())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("date_a")
    ap.add_argument("date_b")
    ap.add_argument("--valve", help="zoom on one valve")
    args = ap.parse_args()

    A = load_snapshot(args.date_a)
    B = load_snapshot(args.date_b)
    off_a, off_b = offset(A), offset(B)

    print(f"\n=== day-over-day diff: {args.date_a} → {args.date_b} ===")
    print(f"  ACS712 offset:  {off_a:.4f} → {off_b:.4f}  (Δ={off_b-off_a:+.4f} A)")
    print()

    valves = sorted(set(A) & set(B))
    if args.valve: valves = [args.valve]

    print(f"  {'valve':<18} {'R_a':>6} {'R_b':>6} {'ΔR':>6}  {'τ_a':>5} {'τ_b':>5}  {'p_a':>5} {'p_b':>5}  flag")
    print("  " + "─"*82)
    for v in valves:
        sa, sb = A.get(v), B.get(v)
        if not sa or not sb: continue
        ra = corr_r(v, sa[-1], off_a)
        rb = corr_r(v, sb[-1], off_b)
        ta, pa = mann_kendall_r(sa, v, off_a)
        tb, pb = mann_kendall_r(sb, v, off_b)
        rs = lambda x: f"{x:6.2f}" if x is not None else "   ---"
        drs = f"{rb-ra:+6.2f}" if (ra is not None and rb is not None) else "   ---"
        flag = ""
        if ra is not None and rb is not None and abs(rb-ra) > 2.0:
            flag += "ΔR"
        if pa > 0.1 and pb < 0.1: flag += " trend-emerging"
        if pa < 0.1 and pb > 0.1: flag += " trend-dissolved"
        if (ta > 0) != (tb > 0) and pb < 0.2: flag += " τ-flipped"
        print(f"  {v:<18} {rs(ra)} {rs(rb)} {drs}  {ta:+.2f} {tb:+.2f}  {pa:.3f} {pb:.3f}  {flag}")
    print()


if __name__ == "__main__":
    main()
