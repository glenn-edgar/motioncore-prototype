# Live state — LaCima irrigation site

Site-specific facts that change over time. Update on each deploy / field change.
Last updated: 2026-07-27.

## Deployed robot
- Image: `nanodatacenter/irrigation-analytics:0.74-self-baseline` (KB2 self-baseline
  coil check — level vs valve's own history so the 4:9–4:12 wire offset cancels;
  cohort kept for drift). Monitor-only: `KB2_COHORT_ARM=0`, `KB2_STEP_ARM=0`.
- Runs on the Pi (`ssh robot` = 192.168.1.66), container `irrigation-analytics`,
  dir `/mnt/ssd/farm/irrigation_analytics/` (SSD, post-06-23 rebuild). Self-recovers
  across reboots.
- WSL/bench instance is normally STOPPED. **Never run it armed while the Pi is armed**
  (double-actuate). One armed instance only.

## Arming state (the gates)
| Knob (in Pi `fleet.env`) | Value | Effect |
|---|---|---|
| `SKIP_LIVE` | `1` | controller writes go LIVE (not dry-run) |
| `KB1_ARM_KILL` | `1` | KB1 overcurrent → CLOSE_MASTER + SKIP |
| `KB3_ARM_KILL` | `1` | KB3 leak → actuate |
| `KB3_WELL_ARM` | `1` | well-drawdown → rpush recharge + SKIP (ARMED 2026-06-14, first live run not yet observed) |
| `FIELD_LOG_ARM` | `1` | field-check action → baseline reset (live-testing) |
| `KB3_HYDRAULIC_ARM` | absent | divergence/well stay monitor-first |

To DISARM any: set the knob `=0` in Pi `fleet.env` + restart. NOTE: `start.sh` passes a
hand-listed `-e` env allowlist — a NEW knob must be added there too, or it won't reach
the container.

## Known field faults (open)
- **4:10 = pipe break (coyote pipe) — REPAIRED 2026-06-14.** It had been held out, but
  ran 06-14 (steps 33/34) above baseline, over-drew the well. Field-repaired; clean
  post-fix runs ~8 GPM (was ~11–12 leaking). `repair_leak` logged → watcher cleared the
  watch; baseline re-learning from clean runs.
- **4:11 = REPAIRED 2026-06-14** (`repair_leak` applied); clean ~6 GPM.
- **4:9 = internal leak (hydraulic).** NOT a shorted coil — the June "shorted-turns
  suspect" note was wrong-signed: 4:9 reads HIGHER R (lower current), not lower. That
  offset is the 200 ft wire spur (see 4:9–4:12 entry below), not a coil fault.
- **1:32 = external leak** (~13 GPM, within sensor range → trustworthy +5 flag; watch-listed).
- **3:11 = sub-floor drip zone, working FINE** — marked `phantom=1` (reads ~0 = no data).
- **4:4** — watch: 5–15 gallons declining (109→101 post-Thu-fix), maybe a new ~2–3 head clog.
- **⭐ 4:9–4:12 = the 200 ft 18 AWG SPUR (Glenn 2026-07-27). Solenoids HEALTHY.**
  This sub-branch hangs off a 200 ft 18-gauge run beyond the 4:1–4:8 manifold. Round
  trip 400 ft × 6.385 Ω/kft = **2.55 Ω** of series wire, which fully explains why
  4:9/4:10/4:11 read R_coil ≈ 28 Ω vs the 4:1–4:8 cluster ≈ 25.7 Ω (+2.3 Ω measured).
  Confirmed by THREE independent methods agreeing on the same offset (during-run derived
  I_coil, LSQ coil_onset decomposition, valve_test proof-of-life) with FLAT onset
  signatures. **Not coil faults, not a corroded terminal — just wire length.** Do NOT
  service these solenoids. The expected R_coil for anything on this spur is cohort+2.5 Ω.
- **4:10 / 4:11 = ACTIVE hydraulic LEAKS (2026-07-25/26).** KB3 leak-curve caught + live
  skip+recharge: 4:11 07-25 (Hunter 11.6 then 14.4 vs 7.3 base), 4:10 07-26 (Hunter
  ramped 5.8→15 GPM through the run vs 9.1 base). Downstream pipe/head — INDEPENDENT of
  the coils (a leak doesn't change coil current). Field-inspect the laterals; leave the
  solenoids.
- **4:12 = re-cleared 2026-07-27.** The 07-18..21 intermittent high-R (34–44 Ω) has NOT
  recurred — live raw ring + valve_test both read it normal/stable now. Its derived data
  looked "frozen" only because its paired flow is very steady. No action.
- **2:16 / 3:14 = NOT faults.** They alerted `R_STEP_DURING_RUN` nightly (17 alerts
  07-18..21); all were the onset-sample artifact. Cohort z ≈ 0, current FALLING
  (healthy). Do not field-service on the strength of those alerts.

## Source / schedule
- **Well was dark 07-17 → 07-27 due to a SCHEDULE CHANGE, not drawdown — Glenn
  corrected it 2026-07-27; well resumes in a few hours (2:7 + deep well pairs return).**
  If you see the 07-17..27 all-city stretch in historical data, it was benign config,
  not a well/dry-run problem. `No_city_water` = the well schedule; `New_check` = the
  city ETO sweep. Don't re-investigate the gap.

## Pending / watch
- **⭐ NEXT BUILD (2026-06-15): KB3 leak CURVE detector.** The 06-14 4:10 break was
  missed because KB3 polls a damped popup channel (saw HUNTER=8.4 flat) once/min vs the
  real raw ~11–12 / PLC ~12–13.4 in the measurement stream. Fix = read the STREAM +
  per-bin RELATIVE baseline (+~3 GPM); leak lift measured at +3–4 GPM. Lowering the
  absolute 14→13 threshold was proven cosmetic and NOT shipped.
- **First live `KB3_WELL_ARM` actuation fired 06-14 on 4:11** (skip + recharge, both
  200, recovered correctly). It catches the downstream symptom, not the leak source.
- Multi-week monitors accumulating (read-time, no thresholds yet): flow_within clog
  trend, coil_decomp per-coil current trend, well-drawdown/internal-leak logs.

## Site-specific tunings (current production values)
Well-drawdown detector: `WARMUP_MIN=5`, `PLATEAU_FROM/TO=5/12`, `DRAW_FRAC=0.78`,
`ONSET_CONSEC=2`, `WINDOW=4`/`WINDOW_HITS=3`, `HUN_DROP=1.5`, `GUARD_REMAIN_MIN=1`,
`IL_DIV_ABS=3.5`, `IL_SUSTAIN=2`. KB2 R calibration: `V_PSU≈15.4`, offset from null
channels `3:1`+`4:6`. KB1: IRR_KILL=1.8 A, EQ_KILL=1.2 A. Master `1:43` ≈ 0.46 A.
KB2 cohort detector (2026-07-21): `KB2_COHORT_Z=3.5`, MAD floors `0.010 A` / `8 mA`,
min deviation `0.05 A`, min rise `15 mA`, trim `KB2_TRIM_FRAC=0.80`, 14-day cohort
lookback. `KB2_COHORT_ARM=0` (monitor-only) and `KB2_STEP_ARM=0` (R_STEP demoted to
log-only — proven artifact).
- **⚠️ Cohort level check has a wiring blind spot — DO NOT ARM as pure cohort.** It
  lumps a whole satellite into one group, but wire-run length shifts R per sub-branch
  (4:9–4:12's 200 ft spur = +2.5 Ω → permanent `COHORT_WEAK_COIL` vs 4:1–4:8). Fix
  (2026-07-27): the LEVEL check now compares each valve to its OWN rolling self-baseline
  (a fixed wire offset is constant, so it cancels; only a MOVE from the valve's own
  history flags). The cohort is kept only for the drift/direction (RISING_I) check, where
  common-mode still cancels. Self-baseline needs ~N runs of history before it scores.
