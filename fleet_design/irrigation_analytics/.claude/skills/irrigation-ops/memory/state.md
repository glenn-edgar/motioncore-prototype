# Live state — LaCima irrigation site

Site-specific facts that change over time. Update on each deploy / field change.
Last updated: 2026-06-14.

## Deployed robot
- Image: `nanodatacenter/irrigation-analytics:0.38-well-arm`.
- Runs on the Pi (`ssh robot` = 192.168.1.66), container `irrigation-analytics`,
  dir `/home/pi/farm/irrigation_analytics/`. Self-recovers across reboots.
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
- **4:9 = internal leak** (also ~6 Ω below same-branch peers = shorted-turns suspect).
- **1:32 = external leak** (~13 GPM, within sensor range → trustworthy +5 flag; watch-listed).
- **3:11 = sub-floor drip zone, working FINE** — marked `phantom=1` (reads ~0 = no data).
- **4:4** — watch: 5–15 gallons declining (109→101 post-Thu-fix), maybe a new ~2–3 head clog.
- **4:12 = SUSPECT high-resistance CONTACT (2026-07-21, current-confirmed).** Cohort
  z=−6.5: I_coil 0.453 A vs sat4 cohort 0.580 A → R_coil 34–38 Ω vs 26.6 Ω. INTERMITTENT
  (per-run 38.4/44.2/37.7/40.3/25.4/26.9 Ω). Rising R ⇒ oxidised relay/contact, NOT a
  shorting coil — chase the connection before replacing the solenoid. See
  [onset artifact + cohort rule](coil-current-onset-artifact.md).
- **2:16 / 3:14 / 3:2+4:12 = NOT faults.** They alerted `R_STEP_DURING_RUN` nightly
  (17 alerts 07-18..21); all were the onset-sample artifact. Cohort z ≈ 0, current
  FALLING (healthy). Do not field-service on the strength of those alerts.

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
