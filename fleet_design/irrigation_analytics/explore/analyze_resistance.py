#!/usr/bin/env python3
"""
KB2-daily resistance analyzer — standalone explore.

Reads:
  data/valve_test.json    — {"satellite_X:pin": [I_A, ...×20]} for 49 valves
  data/valve_groups.json  — 10 sun-exposure groups for sat_1/2/3

Pipeline:
  1. ACS712 per-cycle offset from disconnect-outlier valves (median of latest I)
  2. R_raw = V_supply / (I_latest − offset);  V_supply = 15.5 V
  3. Wire constants: sat_2:13..17 subtract 7 Ω;  sat_1:44 multiply R by 2 (parallel)
  4. Cohort assignment: sat_1/2/3 from JSON + operator's sun-map;
                        sat_4 1..8 shade / 9..16 sun
  5. Cohort z-score on R_corrected
  6. Mann-Kendall trend on each valve's 20-element series (raw I, monotone same as R)
  7. Classification: DEAD / DISCONNECTED / INOPERABLE / PRE_SHORT / AGING_OPEN /
                     IMPENDING_OPEN / STABLE / UNKNOWN_COHORT
  8. Tabular report to stdout
"""

import json
import math
import statistics
from pathlib import Path

V_SUPPLY = 15.5
DISCONNECTS = {"satellite_1:1", "satellite_1:28", "satellite_1:38",
               "satellite_1:40", "satellite_3:1", "satellite_4:6"}
# CALIBRATION (2026-05-26 anchored to physical measurements):
#   New-install solenoid: 43 Ω
#   Anchor A: sat_2:4 physical = 35 Ω (no wire) → ACS712 offset = 0.0133 A
#   Anchor B: sat_2:13..17 physical = 42 Ω (with trunk wire) → wire = 10.07 Ω median
# Disconnect valves are NOT zero-current — they are ~200 Ω high-impedance leakage
# paths that pass ~0.077 A. Use them for daily-drift detection only, not as offset.
ACS712_OFFSET = 0.0133
WIRE_OFFSET_OHMS = {**{f"satellite_2:{p}": 10.0 for p in (13, 14, 15, 16, 17)},
                    **{f"satellite_3:{p}":  3.0 for p in (11, 12, 13, 14, 15, 16, 17, 18)}}
PARALLEL_PAIRS = {"satellite_1:44"}  # apparent R × 2 = per-coil R
MASTER_DEAD = {"satellite_1:38"}     # already in DISCONNECTS — flagged DEAD
# Heavier-duty solenoids with legitimately lower R — bigger coils. Skip absolute
# thresholds; only trend matters.
MASTER_HEAVY = {"satellite_1:43"}    # working master, larger valve w/ filtering

# operator sun-map (group index 1-based per his enumeration):
SUN_MAP = {1: "sun", 2: "sun", 3: "shade", 4: "sun", 5: "sun",
           6: "sun", 7: "sun", 8: "sun", 9: "shade", 10: "shade"}

R_NEW_SPEC    = 43.0   # new-install solenoid baseline
R_INOPERABLE  = 34.0   # ≈ cohort μ − 3σ; below this = physically inoperable
R_IMPENDING   = 47.0   # ≈ cohort μ + 5σ; above this = trending open
TREND_P_THRESHOLD = 0.10  # loose for baseline characterization


def load() -> tuple[dict, list]:
    here = Path(__file__).parent
    valve_test = json.loads((here / "data" / "valve_test.json").read_text())
    valve_groups = json.loads((here / "data" / "valve_groups.json").read_text())
    return valve_test, valve_groups


def build_cohorts(valve_groups: list) -> dict[str, str]:
    cohort: dict[str, str] = {}
    for idx, g in enumerate(valve_groups, start=1):
        sun = SUN_MAP.get(idx, "unknown")
        for io in g.get("io", []):
            ctl = io.get("controller", "")
            pin = io.get("pin")
            if pin is None: continue
            cohort[f"{ctl}:{pin}"] = sun
    # sat_4 rule: 1..8 shade, 9..16 sun
    for p in range(1, 17):
        key = f"satellite_4:{p}"
        if key not in cohort:
            cohort[key] = "shade" if p <= 8 else "sun"
    return cohort


def offset_from_disconnects(valve_test: dict) -> float:
    """Return median of latest disconnect-valve I values — used for *drift detection*,
    not as the calibration offset (the disconnects pass ~200 Ω leakage current, not
    zero, so their median is sensor-bias + leakage, not bias alone).
    """
    latest = []
    for v in DISCONNECTS:
        series = valve_test.get(v)
        if series and isinstance(series, list):
            latest.append(series[-1])
    if not latest:
        raise RuntimeError("no disconnect values found")
    return statistics.median(latest)


def corrected_r(valve: str, i_latest: float, offset: float) -> float | None:
    i_true = i_latest - offset
    if i_true <= 1e-4:
        return None  # effectively no current → disconnected
    r = V_SUPPLY / i_true
    r -= WIRE_OFFSET_OHMS.get(valve, 0.0)
    if valve in PARALLEL_PAIRS:
        r *= 2.0
    return r


def mann_kendall(series: list[float]) -> tuple[float, float, float]:
    """Returns (tau, z, p_two_tailed). p via standard normal approx."""
    n = len(series)
    s = 0
    for i in range(n - 1):
        for j in range(i + 1, n):
            d = series[j] - series[i]
            s += (1 if d > 0 else (-1 if d < 0 else 0))
    var_s = n * (n - 1) * (2 * n + 5) / 18.0
    if s > 0: z = (s - 1) / math.sqrt(var_s)
    elif s < 0: z = (s + 1) / math.sqrt(var_s)
    else: z = 0.0
    p = math.erfc(abs(z) / math.sqrt(2))
    tau = s / (n * (n - 1) / 2.0) if n > 1 else 0.0
    return tau, z, p


def classify(valve: str, r_corr: float | None, cohort: str,
             z_score: float | None, mk_p: float, mk_tau: float) -> str:
    if valve in MASTER_DEAD: return "DEAD_MASTER"
    if valve in DISCONNECTS: return "DISCONNECTED"
    if r_corr is None:       return "OPEN_CIRCUIT"
    if valve in MASTER_HEAVY:
        # Skip absolute-R thresholds — these are bigger coils. τ on R-series:
        # +τ = R rising = trending toward open; −τ = R falling = trending toward short.
        if mk_p < TREND_P_THRESHOLD and mk_tau > 0: return "HEAVY_TREND_UP"
        if mk_p < TREND_P_THRESHOLD and mk_tau < 0: return "HEAVY_TREND_DOWN"
        return "STABLE_HEAVY"
    if r_corr < R_INOPERABLE:    return "INOPERABLE"
    if mk_p < TREND_P_THRESHOLD:
        if mk_tau > 0:                          return "AGING_OPEN"   # R rising
        if mk_tau < 0 and r_corr < R_IMPENDING: return "PRE_SHORT"    # R falling
    if r_corr > R_IMPENDING:     return "IMPENDING_OPEN"
    if cohort == "unknown":      return "STABLE_UNK_COHORT"
    if z_score is not None and abs(z_score) > 2.0:  return "COHORT_OUTLIER"
    return "STABLE"


def main() -> None:
    valve_test, valve_groups = load()
    cohort_map = build_cohorts(valve_groups)
    leakage = offset_from_disconnects(valve_test)  # drift indicator, not offset
    offset = ACS712_OFFSET  # physical-anchored calibration

    rows = []
    for valve, series in sorted(valve_test.items()):
        if not series: continue
        latest = series[-1]
        cohort = cohort_map.get(valve, "unknown")
        r = corrected_r(valve, latest, offset)
        # MK on R series (semantically clear: τ>0 = R rising = AGING_OPEN;
        # τ<0 = R falling = PRE_SHORT). Skip disconnected/open valves.
        r_series = []
        for i in series:
            ri = corrected_r(valve, i, offset)
            if ri is not None and 5.0 < ri < 500.0:
                r_series.append(ri)
        if len(r_series) >= 5:
            tau, z, p = mann_kendall(r_series)
        else:
            tau, z, p = 0.0, 0.0, 1.0
        rows.append({"valve": valve, "latest_A": latest, "r_corr": r,
                     "cohort": cohort, "mk_tau": tau, "mk_p": p,
                     "series": series})

    # cohort stats over non-disconnect, non-special valves
    skip = (DISCONNECTS | MASTER_DEAD | MASTER_HEAVY | PARALLEL_PAIRS
            | set(WIRE_OFFSET_OHMS))
    cohort_stats: dict[str, tuple[float, float, int]] = {}
    for cname in ("sun", "shade"):
        rs = [r["r_corr"] for r in rows
              if r["cohort"] == cname and r["r_corr"] is not None
              and r["valve"] not in skip]
        if len(rs) >= 2:
            mu = statistics.mean(rs); sd = statistics.pstdev(rs)
            cohort_stats[cname] = (mu, sd, len(rs))

    for r in rows:
        c = r["cohort"]; rv = r["r_corr"]
        if c in cohort_stats and rv is not None and r["valve"] not in skip:
            mu, sd, _ = cohort_stats[c]
            r["z"] = (rv - mu) / sd if sd > 1e-6 else 0.0
        else:
            r["z"] = None
        r["status"] = classify(r["valve"], rv, c, r["z"], r["mk_p"], r["mk_tau"])

    # ── REPORT ────────────────────────────────────────────────────────────
    print(f"\n=== IRRIGATION VALVE RESISTANCE — KB2-daily baseline ===")
    print(f"  ACS712 offset:        {offset:.4f} A  (physical-anchored to sat_2:4=35Ω)")
    print(f"  Disconnect leakage:   {leakage:.4f} A  (drift-indicator only)")
    print(f"  V_supply:             {V_SUPPLY} V")
    print(f"  New-install spec:     {R_NEW_SPEC} Ω")
    print(f"  Trend threshold:      p < {TREND_P_THRESHOLD}")
    for c, (mu, sd, n) in cohort_stats.items():
        print(f"  Cohort {c:<5}    μ={mu:6.2f}Ω  σ={sd:5.2f}Ω  n={n}")

    print("\n  valve              I_latest   R_corr   cohort  z      MK_tau  MK_p    status")
    print("  " + "─" * 88)
    by_status = sorted(rows, key=lambda r: (
        0 if r["status"] not in ("STABLE", "STABLE_UNK_COHORT") else 1,
        r["valve"]))
    for r in by_status:
        rv = f"{r['r_corr']:6.2f}" if r["r_corr"] is not None else "  ---"
        zv = f"{r['z']:+5.2f}" if r["z"] is not None else "  ---"
        print(f"  {r['valve']:<18} {r['latest_A']:7.4f}  {rv}    "
              f"{r['cohort']:<6} {zv}  {r['mk_tau']:+5.2f}  {r['mk_p']:5.3f}  {r['status']}")

    # anomaly summary
    anomalies = [r for r in rows if r["status"] not in
                 ("STABLE", "STABLE_UNK_COHORT", "DISCONNECTED", "DEAD_MASTER")]
    print(f"\n  → {len(anomalies)} anomalies of {len(rows)} valves")
    print(f"  → {sum(1 for r in rows if r['status']=='DISCONNECTED')} disconnects (offset refs)")
    print(f"  → {sum(1 for r in rows if r['status']=='DEAD_MASTER')} dead master(s)\n")


if __name__ == "__main__":
    main()
