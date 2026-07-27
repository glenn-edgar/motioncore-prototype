# The onset artifact + the cohort rule (2026-07-21)

Two bugs that between them made KB2's within-run coil detector report the
**opposite of physical reality** for four months, plus the detector that
replaces it. Refuted hypothesis, confirmed root cause, shipped fix.

---

## 1. The onset sample inverts the sign of the current trend

`R_zone = 1/(1/R_total − 1/R_master)` with `R_total = V_PSU/(I − offset)` is a
difference of two close reciprocals. It is **violently nonlinear** — a small
current dip becomes a huge apparent resistance.

The first sample(s) of a run are taken while the valve is still energizing.
Sometimes before ANY coil is on, where `I_obs ≈ null_offset`.

Worked example, `satellite_1:39/satellite_3:14`, run of 2026-07-21:

| sample | I (A) | derived R (Ω) |
|---|---|---|
| 0 (onset) | 0.727 | **123.7** |
| 1 (steady) | 1.017 | 37.2 |
| end | 1.004 | 38.4 |

`analyze_run`'s sanity filter is `v > 10 and v < 200`, so **123.7 passes** and
lands in the R_start window. Consequences:

- `R_start` inflated 37.6 → 51.9 Ω, which **flips the sign of `end_delta`**:
  stored −13.3 Ω (R falling = current RISING = shorting coil) when the raw
  current actually FALLS 1.012 → 1.004 A (R rising = copper warming = healthy).
- `max_step` = `R(0.727) − R(1.017)` = **86.5 Ω**, which is exactly the
  `R_STEP_DURING_RUN ΔR=86.8 Ω jump at minute 2` alert text.

**Every R_STEP alert over 2026-07-18..21 was this artifact** — 17 alerts, 3
bins (2:16, 3:14, 3:2/4:12), zero real faults. All three sit at cohort median
with falling current.

`procedures/solenoid-health.md` already stated both halves of the rule —
"onset current spike is a red herring" and "the R = V/(I−offset) division is
unstable" — the within-run path just wasn't honoring them.

**Fix:** `KB2_WR.trim_energize()` drops leading/trailing samples below 80% of
the run's median current, applied to `I_data` BEFORE deriving R. On the 3:14
trace: `end_delta −13.30 → +0.93`, `max_step 86.5 → 2.5`, `cls
R_STEP_DURING_RUN → OK`.

⚠️ **Do not diagnose a solenoid from `R_start`/`end_delta` on any row written
before 2026-07-21** — the sign may be inverted. Recompute from the raw
`IRRIGATION_CURRENT` array.

---

## 2. TIME_HISTORY keeps two key orderings and only one is live

The controller stores multi-part bins under BOTH `1:39/X` and `X/1:39`, and
only one is still being appended to. Which one is **inconsistent per bin**:

| bin | `1:39`-first | `1:39`-last |
|---|---|---|
| `3:14` | **8 rings (dead)** | 49 rings (live) |
| `2:16` | 32 rings (stale) | 49 rings (live) |
| `3:2/4:12` | 49 rings (live) | 2 rings (dead) |

`fetch_th` took the exact-name hit, so for 3:14 it re-derived the **same dead
run every night**. Signature: bit-identical `R_start`/`R_end` to 15 significant
figures across consecutive days, with `n_samples` pinned constant (56 for 3:14,
59 for 2:16) even though run lengths varied 48–59 min. Analog measurements
cannot repeat like that — **identical floats across days means a stale ring,
not a stable valve.**

This also **defeated the 2-consecutive-run gate** on R_STEP: the "same" step
recurred forever, so the gate always passed.

**Fix:** `fetch_th` enumerates all orderings and picks the one with the most
rings. Never assume a fixed ordering.

---

## 3. Cohort-relative detection (the replacement)

Absolute R thresholds cannot separate a bad valve from a long cable run:

| group | valves | I_coil median | MAD | R_coil |
|---|---|---|---|---|
| sat2 | 4 | 0.540 A | **0.003** | **28.5 Ω** |
| sat3 | 6 | 0.585 A | 0.008 | 26.3 Ω |
| sat4 | 8 | 0.580 A | 0.020 | 26.6 Ω |

**All four sat2 valves sit ~8% higher R than the other branches, agreeing with
each other to 0.003 A.** That is satellite-2's cable, not four faults — an
absolute threshold tuned on sat3/sat4 flags the entire branch.

Within one satellite the master coil, null offset, PSU rail and branch wiring
are common-mode, so compare each valve to ITS OWN GROUP:

```
I_coil = I_observed − null_offset − I_master      (I_master = V_PSU/R_master ≈ 0.369 A)
flag when |robust z vs group median| > 3.5
LOW vs cohort  -> weak coil / high-R connection (oxidised contact)
HIGH vs cohort -> shorted turns
```

**Two physical guards are mandatory** — without them a tight cohort collapses
the MAD and a 4 mA spread scores z = ±4451 (sat2 produced two phantom faults
on the first pass):

- MAD floor `0.010 A` — the sensor quantises at ~6.5 mA; finer "precision" is fake.
- Minimum deviation `0.05 A` (~9% of a coil) — a finding must be statistically
  AND physically large.
- Rising-current needs a real rise (`≥15 mA`), not merely more than peers.

With both guards, 18 ETo valves → **17 OK, 1 finding**.

---

## What this found: 4:12

**`satellite_4:12` — COHORT_WEAK_COIL, z = −6.5.** I_coil 0.453 A vs sat4
cohort 0.580 A → **R_coil 34–38 Ω vs 26.6 Ω**, +30–43%.

**Intermittent**, per-run on its live ring (oldest → newest):
`38.4  44.2  37.7  40.3  25.4  26.9 Ω` — four bad runs then two clean.

Intermittent HIGH resistance ⇒ **relay/contact oxidation, not a shorting coil**
(shorted turns read LOWER). Chase the connection before swapping the solenoid.
Flow neither confirms nor refutes: 4:12 only runs paired with 3:2 in the ETo
schedule and that bin is at baseline (6.5–6.8 GPM).

Cleared by the same pass: **2:16, 3:14, 3:2/4:12** (the three that alerted
nightly) all z ≈ 0 with falling current. **4:11** looked like rising current
(+91.5 mA) on a single untrimmed run — it is normal (−16.5 mA) over 6 trimmed
runs. One run is never enough for a drift call.

---

## Method note for the next operator

When a current/resistance finding disagrees with what the raw trace shows,
**trust the raw `IRRIGATION_CURRENT` array**, not the derived R. Pull it with
`references/data-access.md` § TIME_HISTORY and use `.data`. Falling current
through a run is the healthy state; the derived-R pipeline has now inverted
that twice.
