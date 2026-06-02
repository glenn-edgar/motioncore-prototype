# irrigation_analytics — CONTINUE

Pick-up doc for the parallel KB1/KB2/KB3/KB4 build. Read first on any
session resume.

**Strategy (locked 2026-05-29 evening, Glenn):** build KB2/KB3/KB4 as
**minimized chain_tree modules** running in parallel inside the robot.
Validate analysis math against organic operator cycles first. Defer
Zenoh + dashboard + persistence-service + RPC integration until all
four KBs' analysis is bench-green.

Memory anchor: `parallel-minimized-chain-trees-2026-05-29`.

---

## Current state (EOD 2026-05-29)

### KB1 — shadow robot (LIVE)

- **Status:** bare-LuaJIT shadow at `robot/` running continuously in WSL.
  PID 21518 at session wrap. Discord on (real channel, `[KB1 SHADOW]`
  prefix). No SKIP_STATION / CLOSE_MASTER_VALVE — observation only.
- **Logs:** `robot/var/kb1.log` (per-poll JSON), `robot/var/kb1_events.log`
  (per Discord event), `robot/var/last_stream_id` (past_actions cursor).
- **Open verification:** 120 s warm-up gate shipped 22:30 PT 2026-05-29
  but UNVERIFIED — first-morning job is to confirm zero MODE_1_LOW /
  MASTER_IDLE_LOW fires within 120 s of any overnight ACTIVE_RUN /
  MASTER_IDLE_CHECK entry. Recipe in
  `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/irrigation_analytics_daily_review_workflow.md`.
- **Restart command:**
  ```
  cd robot && pkill -f 'luajit.*main.lua' || true; POLL_INTERVAL_S=10 ./run.sh &
  ```
- **Memory:** `kb1-design-locked-2026-05-29`,
  `irrigation-analytics-daily-review-workflow`.

### KB2 — post-event resistance analyzer (NOT IN ROBOT YET)

- **Status:** explore-side Python proven. Adaptive-baseline /
  cohort-residual / onboard-new-valve logic locked in design but not
  coded.
- **Scaffold scripts** (read-only after KB2 lands):
  - `explore/analyze_resistance.py` — MK-trend + family-residual + cohort z;
    `RECENT_N=2` median-of-last-2 (cuts within-day noise 16%).
  - `explore/stitch_r_history.py` — daily snapshot stitcher.
  - `explore/compare_snapshots.py` — cross-day diff.
- **Wired in KB1 already:** `kb2.run_resistance_analysis` enable on
  `RESISTANCE_CHECK → not-RESISTANCE_CHECK` edge; `kb2.update_curve` on
  every `IRRIGATION_STEP_COMPLETE`. KB2 just receives the enable.
- **Memory:** `kb1-design-locked-2026-05-29` (KB1↔KB2 plumbing section);
  `solenoid-failure-research-2026-05-26`;
  `irrigation-analytics-explore-state-2026-05-26`.
- **Build estimate:** 2 days when started.

### KB3 — real-time flow monitor (NOT STARTED)

- **Status:** design analogous to KB1 (mirror, flow side). Not coded.
- **Day-1 task:** verify popup has a live flow field (HUNTER_FLOW_METER
  or similar). KB1 only reads PLC_IRRIGATION_CURRENT + PLC_EQUIPMENT_CURRENT
  from popup today; flow may or may not be there. If yes → mirror KB1
  poll loop. If no → KB3 has to read TIME_HISTORY periodically (different
  pattern from KB1, more like KB4).
- **Modes:** mirror KB1 — low warn (Discord), high warn (Discord), trip
  (CLOSE_MASTER_VALVE for runaway flow = pipe break), fixed-limit safety.
- **Build estimate:** 1-2 days when started.

### KB4 — post-event flow + current fusion (NOT IN ROBOT YET)

- **Status:** explore-side Python proven today on 48 short runs. 6 raw
  flags reduced to 2 real escalations once we layered in user context +
  adaptive baseline thinking + cohort layer. See "Today's findings"
  below.
- **Scaffold scripts** (read-only after KB4 lands):
  - `explore/today_last_sample.py` — end-current vs kb1_thresholds μ.
  - `explore/today_flow_endpoint.py` — end-flow vs per-bin med ± MAD.
- **Key analysis features KB4 must own** (lessons from today):
  1. **Two-stream fusion** at `IRRIGATION_STEP_COMPLETE` — combine end-flow
     Δ and end-current Δ; sign pattern → failure-mode label (low+low=CLOG,
     high+low=BREAK+coil-aging, high+normal=pure BREAK, zero+normal=
     upstream block).
  2. **Adaptive baseline** — when N≥3 consecutive runs sit at a new
     stable level outside historic MAD, declare new baseline. Without
     this, sat_3:4 / sat_3:19 (today) fire CLOG/BREAK forever after a
     repair. Memory: same lesson logged in
     `kb1-design-locked-2026-05-29` KB3/KB4 sufficiency section.
  3. **Cohort / family-residual** — if ≥3 valves on same satellite in
     recent steps all show negative Δ, escalate as cohort alert (sat_4
     well-pressure pattern today: 3 of 4 didn't cross individual gate
     but cohort signal was clear).
  4. **Within-run scan** for `run_time ≥ 8 min` (mid-run regime change;
     `explore/within_run_scan.py` has the math).
  5. **ETO truncation handling** — `arming.eto_restriction_seen=true` →
     skip curve update; sample is biased toward early-phase.
  6. **`has_flow_signal` flag** — false for sat_1:1, 1:17, 1:28, 1:39,
     1:40 (city-water cohort, no Hunter flowmeter reading).
  7. **Bin class** — `eto_irrigation` vs `landscape` from
     `eto_site_setup.json` membership. Drives which chains fire.
- **Memory:** `irrigation-schedule-taxonomy-2026-05-29`,
  `kb1-design-locked-2026-05-29` (KB3/KB4 sufficiency section).
- **Build estimate:** 3-5 days when started.

---

## Today's findings (2026-05-29 PM) — design-shaping data

48 short runs (5–45 min, all `run_time` is MINUTES not seconds). Pulled
from past_actions XRANGE since midnight Pacific, cross-referenced
against TIME_HISTORY (last-N runs per bin).

### 6 raw flow flags → triage with operator context

| Bin | Class | Raw flag | Real diagnosis |
|---|---|---|---|
| sat_4:10 | ETO | CLOG | **Well-pressure droop in sat_4 cohort.** All 4 sat_4 ETO valves (4:9, 4:1, 4:10, 4:11) under-baseline 1.3-2.3 GPM in consecutive steps 30-34. Glenn adjusting timing to help. Verify tomorrow. |
| sat_3:11 | landscape | CLOG (0 GPM) | **Throw-away well-charge step** — schedule fixed at source (replaced with sat_1:39). Was BY DESIGN, not anomaly. |
| sat_3:4  | landscape | CLOG | **Step-change false positive.** Run 43 (~mid-May) dropped from 9 GPM → 7 GPM and stayed flat 6 runs. Repair / nozzle change. Adaptive baseline kills this. |
| sat_3:19 | landscape (right_bank) | BREAK | **Step-change false positive.** Run 43 (~mid-May) rose 3.3 GPM → 6 GPM and stayed flat. **Popped pivot head** (Glenn confirmed). Same fix as sat_3:4. |
| sat_2:2  | landscape | BREAK 22 GPM | **Real — worsening.** AM run 16.7 GPM, PM run 22 GPM, baseline 12.3. Within-today escalation = genuine signal. |
| sat_2:17 | landscape | low | Mild, noise-floor. |

**Cross-validation: sat_2:6 (NOT flagged) is the gold-standard true
negative.** Dashboard shows converging ~12 GPM; analyzer hist_med=11.3
and |z|=1.82 (no flag). Confirms the analyzer correctly distinguishes
healthy from anomalous when baseline is stable.

### Two real escalations

1. **sat_4 ETO cohort well-pressure** — needs cross-bin layer (KB4 must own).
2. **sat_2:2 worsening landscape BREAK** — needs trend-within-day rule.

### Three KB4 design must-haves crystallized

- **Adaptive baseline** (3 of 6 false positives traceable to this).
- **Cohort/family-residual** (1 real positive missed by single-valve threshold).
- **Two-stream fusion** (every failure-mode label needs both flow + current sign).

---

## Daily-cadence routine

Each morning before any code change:

1. **Verify robot still running:**
   `ps -ef | grep 'luajit.*main.lua' | grep -v grep` — should see PID.
2. **Today's slice + tonight's overnight events:**
   ```
   cd robot
   TODAY=$(date -u +%Y-%m-%d)
   grep "\"t\":\"$TODAY" var/kb1.log > /tmp/kb1_today.jsonl
   wc -l var/kb1_events.log    # any new Discord events?
   ```
3. **Check 120 s warm-up worked:** grep `var/kb1_events.log` for
   MODE_1_LOW / MASTER_IDLE_LOW. Should be 0 within 120 s of any
   IDLE → ACTIVE_RUN / IDLE → MASTER_IDLE_CHECK edge.
4. **Rerun yesterday's analyzers if irrigation ran overnight:**
   ```
   cd explore
   python3 today_last_sample.py    # end-current vs μ
   python3 today_flow_endpoint.py  # end-flow vs per-bin med ± MAD
   ```
5. **One fix per morning.** Pick the highest-priority issue from the
   overnight log; implement; restart robot; let it bake another day.

---

## Pickup order for next sessions

In priority for the next ~5 days:

1. **Tomorrow morning (2026-05-30):** daily review per above. Verify
   warm-up gate worked overnight. Check sat_4 cohort post-timing-fix.
2. **Tomorrow/+1:** KB3 minimum (verify popup live-flow field; if yes,
   mirror KB1 poll + 4 modes + Discord).
3. **+2:** KB4 v1 — end-flow + end-current fusion at STEP_COMPLETE,
   JSON-backed baselines, no adaptive logic yet (flag everything, tune
   day-by-day).
4. **+3:** KB4 adaptive baseline + cohort layer (today's lessons).
5. **+4:** KB4 within-run scan for ≥8-min runs + ETO truncation gate.
6. **+5:** KB2 chain_tree port of `analyze_resistance.py` + adaptive
   baseline + new-valve onboarding + cohort. (Or earlier if KB3/KB4
   timing slips.)

**Defer to integration phase (post-validation, ~+7):** Zenoh leaves,
dashboard widgets, application_gateway RPCs, SQLite/persistence-service
migration, flipping shadow-mode gates so KB2/KB4 can actually act.

---

## File map (for next session)

```
fleet_design/irrigation_analytics/
├── CONTINUE.md                              # THIS FILE
├── docs/                                    # README, design docs
│   └── README.md                            # solenoid failure research
├── robot/                                   # SHADOW LIVE
│   ├── main.lua                             # KB1 poll loop
│   ├── run.sh                               # launcher (LUA_CPATH for cjson)
│   ├── lib/
│   │   ├── controller.lua                   # SSH+Python+Redis popup/past_actions
│   │   ├── state_machine.lua                # 6-state classifier
│   │   ├── modes.lua                        # eval_mode4/eq/calibrated/master_idle
│   │   ├── thresholds.lua                   # V/R/IRR_TRIP_A/EQ_*/WARMUP_S constants
│   │   ├── discord.lua                      # wrapper over notification_service
│   │   └── logger.lua                       # JSON-per-line append writer
│   └── var/                                 # logs + cursor (in .gitignore)
└── explore/                                 # SCAFFOLD (Python; goes read-only when KB2/4 lands)
    ├── analyze_resistance.py                # → KB2
    ├── stitch_r_history.py                  # → KB2
    ├── compare_snapshots.py                 # → KB2
    ├── extract_bin_baselines.py             # → KB2 (curve generation)
    ├── derive_kb1_thresholds.py             # → KB1 (already shipped)
    ├── default_curve.py                     # → KB2 (cohort seeds)
    ├── within_run_scan.py                   # → KB4 (within-run regime scan)
    ├── within_run_topN.py                   # → KB4 (within-run flagging)
    ├── short_run_end_score.py               # → KB4 (FLOW end-only, older variant)
    ├── today_last_sample.py                 # → KB4 (end-current today's-runs analyzer; committed 2026-05-29)
    ├── today_flow_endpoint.py               # → KB4 (end-flow today's-runs analyzer; committed 2026-05-29)
    ├── kb1_thresholds.json                  # KB1 input (needs 1.75 A trip re-derivation)
    ├── stitched_r_history.json              # KB2 stitched daily R-history
    └── snapshots/YYYY-MM-DD/                # daily snapshot tree
```

## Memory anchor map

- `kb1-design-locked-2026-05-29` — KB1 full spec + KB3/KB4 sufficiency lessons
- `irrigation-schedule-taxonomy-2026-05-29` — ETO vs landscape, ETO truncation, has_flow_signal cohort
- `parallel-minimized-chain-trees-2026-05-29` — build sequencing strategy
- `irrigation-analytics-daily-review-workflow` — morning review recipes
- `solenoid-failure-research-2026-05-26` — physics anchors backing KB2 trip thresholds
- `new-irrigation-robot-design-2026-05-26` — historical (KB1 portion superseded)
- `irrigation-analytics-explore-state-2026-05-26` — earlier explore artifacts
