# Operator memory — index

This is YOUR persistent store (the operator agent's), distinct from the procedures.
One line per memory; detail lives in the linked file. Read `state.md` every session —
it's the live state of the site and changes day to day.

- [Live state](state.md) — current image, what's armed, known field faults, watch list. READ FIRST.
- ⭐ [Onset artifact + cohort rule](coil-current-onset-artifact.md) — the derived-R pipeline reported the OPPOSITE of the raw current for months. Onset sample → R=123 Ω phantom → inverted `end_delta` + every `R_STEP` alert false; TIME_HISTORY keeps two key orderings and only one is live (identical floats across days = stale ring). Replacement = cohort-relative coil current. Found 4:12 (intermittent high-R contact). Trust the raw `IRRIGATION_CURRENT` array over derived R.

<!--
Add a line above for each durable fact you learn about THIS site. Keep procedures
generic (in ../procedures); keep site-specific live facts here. Format:
- [Title](file.md) — one-line hook
-->
