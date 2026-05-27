#!/usr/bin/env python3
"""
Build a per-valve R-history series from the daily resistance scan.

Output: stitched_r_history.json
  {
    "satellite_X:pin": {
      "cold":     [{"ord": int, "r": float, "source": "cold_d<n>"}, ...],
      "stitched": [{"ord": int, "r": float, "source": "cold"}, ...]
    }
  }

Ordinal scale:
  ord = 0   → today (newest cold-R)
  ord = -n  → n days ago

Hot-R from time_history runs is intentionally NOT included here. Run current
is parallel(zone, master) ≈ 20Ω, not single-valve. Including it produces
spurious "jumps" at the hot/cold seam. See memory:
[[master-valve-parallel-correction-2026-05-27]].
"""
import json
import statistics
from pathlib import Path
from collections import defaultdict

V_SUPPLY = 15.5
ACS712_OFFSET = 0.0133
NONEXISTENT = {"satellite_1:1", "satellite_1:28", "satellite_1:38",
               "satellite_1:40", "satellite_3:1", "satellite_4:6"}  # 6 phantom null-anchors
WIRE_OFFSET_OHMS = {**{f"satellite_2:{p}": 10.0 for p in (13, 14, 15, 16, 17)},
                    **{f"satellite_3:{p}":  3.0 for p in (11, 12, 13, 14, 15, 16, 17, 18)}}
PARALLEL_PAIRS = {"satellite_1:44"}


def corr_r(valve: str, i_amps: float) -> float | None:
    """Convert measured current to corrected R. None if invalid."""
    i_true = i_amps - ACS712_OFFSET
    if i_true <= 1e-4:
        return None
    r = V_SUPPLY / i_true - WIRE_OFFSET_OHMS.get(valve, 0.0)
    if valve in PARALLEL_PAIRS:
        r *= 2.0
    if not (5.0 < r < 500.0):
        return None
    return r


def asymptote(samples: list[float]) -> float | None:
    """Mean of last 3 non-zero samples — skip sample[0] timing-race per characterize_runs."""
    # Trim trailing zeros (post-close)
    n = len(samples)
    while n > 0 and samples[n-1] <= 1e-6:
        n -= 1
    samples = samples[:n]
    # Skip sample[0] — known timing race (σ=0.153 vs 0.005 at sample[1])
    if len(samples) < 4:
        return None
    tail = samples[-3:]
    return statistics.mean(tail)


def main():
    here = Path(__file__).parent
    vt = json.loads((here / "data" / "valve_test.json").read_text())

    out: dict[str, dict] = {}
    for valve, daily_series in vt.items():
        cold: list[dict] = []
        n_cold = len(daily_series)
        for i, i_amps in enumerate(daily_series):
            ord_pos = -(n_cold - 1 - i)
            r = corr_r(valve, i_amps)
            if r is None:
                continue
            cold.append({"ord": ord_pos, "r": round(r, 3),
                         "source": f"cold_d{-ord_pos}"})

        stitched = [{"ord": e["ord"], "r": e["r"], "source": "cold"}
                    for e in cold]
        out[valve] = {"cold": cold, "stitched": stitched,
                      "n_cold": len(cold), "n_total": len(stitched)}

    out_path = here / "stitched_r_history.json"
    out_path.write_text(json.dumps(out, indent=2))

    print(f"\n=== R-history (cold-R only, 20-day window) ===")
    print(f"  valves: {len(out)}")
    if "satellite_2:4" in out:
        print(f"\n  sat_2:4 (known recently-replaced) full series:")
        for r in out["satellite_2:4"]["stitched"]:
            print(f"    ord={r['ord']:>4}  R={r['r']:6.2f}")
    print(f"\n  → {out_path}")


if __name__ == "__main__":
    main()
