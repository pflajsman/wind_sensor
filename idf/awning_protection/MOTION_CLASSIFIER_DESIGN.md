# Motion classifier — design spec (NOT yet implemented)

Goal: on a **front-bar mount**, tell a **deliberate motor open/close** apart from **wind
disturbance**, so the everyday open/close does not falsely latch occupancy = 1.

Status: **design only — no code written.** Review this, then measure the metrics on the real
awning (see §6) before we implement. Nothing in the current firmware changes until you say so.

Related: [`TECHNICAL.md`](TECHNICAL.md) (current motion-latch model), [`README.md`](README.md)
(sensitivity), the field notes on why the Faraday-cage and tilt approaches were rejected.

---

## 1. Why this approach

Prior approaches were ruled out on hardware:

- **Faraday-cage as signal** (closed awning shields the radio → "disconnect = closed"):
  **rejected** — when closed the sensor is only ~1.2 m from Router A, so proximity beats the
  aluminium and the link actually *stays strong*. No disconnect happens.
- **Tilt-change** (arm rotates open vs closed): **rejected for this mount** — the sensor is on
  the **front bar**, which mostly translates without a clean, large resting-angle change.

What *does* differ on a front bar is the **character of the motion**:

| | Motor open/close | Wind |
|---|---|---|
| Velocity | steady, ramps up then down once | erratic, gusty |
| Direction | one dominant direction of travel | oscillating, back-and-forth |
| Acceleration signal | smooth, low-frequency, few sign changes | jerky, high-frequency, many sign changes |
| Energy across axes | concentrated on the travel axis | spread across axes |
| Duration | bounded (~36 s here), then a hard stop into stillness | unbounded, intermittent, no clean stop |

The classifier reads these features from a short measurement window and decides
**smooth/regular → opening (keep occupancy 0)** vs **chaotic/oscillating → wind (occupancy 1)**.

---

## 2. Where it slots into the current firmware

Today (`report_task`, see TECHNICAL.md §4): ADXL activity fires → occupancy latches 1
immediately. The classifier inserts **one step between "activity fired" and "latch 1"**:

```
activity fires (ADXL INT / poll)
        │
        ▼
  run classification window  (§3)   ← NEW
        │
   ┌────┴─────────────┐
 SMOOTH             CHAOTIC
 (opening)          (wind)
   │                  │
 keep occ 0       latch occ 1  (existing behaviour)
 (start/continue
  a "move" grace)
```

The occupancy **clear** logic (30 s quiet → 0) is unchanged.

Important: this **re-introduces a measurement window on the motion path**, which we previously
removed. The critical difference — we are **not** thresholding average RMS (that averaged brief
shakes to nothing). We classify the *pattern*, and the window is **short** (see §3), run only
when motion is already occurring. It is a classifier, not an energy gate.

---

## 3. The classification window

On an activity trigger, sample the ADXL at high ODR for a short window and compute features.

- **ODR:** 50 Hz (driver already has `adxl345_set_odr50()`).
- **Window length `CLASS_WINDOW_MS`:** start at **1500 ms** (75 samples). Long enough to see
  several wind oscillations (wind is typically 1–5 Hz flutter) and to characterise smooth
  travel; short enough that occupancy still reacts within ~1.5 s. (Old broken value was 4000 ms.)
- Use the **AC component** of each axis (subtract the per-axis mean over the window) so gravity
  and static tilt drop out — same idea as the ADXL's AC-coupled activity.

### Candidate metrics (compute all; pick the discriminating subset after measuring)

1. **Direction-reversal count `reversals`** — on the dominant axis (largest AC variance), count
   sign changes of the AC signal across the window. *Smooth travel → few (a handful); wind →
   many.* Likely the single best, cheapest metric.
2. **Mean jerk `jerk`** — mean of |sample[i] − sample[i−1]| on the dominant axis (or vector).
   *Smooth → low; gusty → high.* Captures high-frequency content without an FFT.
3. **Axis dominance `dom_ratio`** — dominant-axis AC energy ÷ total AC energy (0..1). *Travel →
   high (one axis); wind → lower (spread).*
4. **Net displacement vs path `directionality`** — |sum of AC deltas| ÷ sum of |AC deltas| on
   the dominant axis (0..1). Steady one-way motion → near 1; oscillation → near 0. (A cleaner,
   normalised cousin of reversal-count.)

### Decision rule (initial, refine with data)

Classify as **OPENING (smooth)** when it looks like steady one-way travel, else **WIND**:

```
smooth =  reversals   <= REVERSALS_MAX
      &&  jerk        <= JERK_MAX
      && (directionality >= DIRECTIONALITY_MIN  ||  dom_ratio >= DOM_RATIO_MIN)
```

- `smooth == true`  → treat as deliberate move: **do not set occupancy 1** (see §4 grace).
- `smooth == false` → **wind**: latch occupancy 1 (current behaviour).

Keep it a small boolean combination of measured thresholds, not a black box, so it stays
tunable and debuggable. Start with reversal-count + directionality as the primary pair; add
jerk / dom_ratio only if the first pair doesn't separate cleanly on real data.

---

## 4. Duration bound (the 36 s move envelope)

A motor move is **bounded ~36 s then stops dead**; wind is not. Use this as a safety net so a
long steady gust can't hide behind "smooth" forever:

- On the first "smooth" classification, start a **move grace window** of
  `MOVE_MAX_S = 45 s`  (36 s travel + margin).
- While inside the grace window and motion keeps classifying **smooth**, keep occupancy 0.
- If motion is **still present after `MOVE_MAX_S`**, a real open/close would have finished by
  now → reclassify as **wind → latch occupancy 1** (a sustained disturbance is wind, whatever
  its texture).
- If motion **stops** before `MOVE_MAX_S` (a clean end into stillness) → it was a move; stay 0.

This bounds the worst case: the most a genuine wind event can be suppressed is one `MOVE_MAX_S`
(≈45 s) *and only if* it also looked smooth the whole time — unlikely for real gusty wind.

---

## 5. New tunables (proposed, in `awning_config.h`)

| Name | Start value | Meaning |
|---|---|---|
| `CLASS_WINDOW_MS` | 1500 | classification window length |
| `MOVE_MAX_S` | 45 | max time a "smooth" burst is treated as an open/close (36 s travel + margin) |
| `REVERSALS_MAX` | *TBD* | max dominant-axis sign changes to still count as smooth |
| `JERK_MAX` | *TBD* | max mean jerk (mg/sample) to still count as smooth |
| `DIRECTIONALITY_MIN` | *TBD* | min net/path ratio for one-way travel |
| `DOM_RATIO_MIN` | *TBD* | min dominant-axis energy fraction |

The `*TBD*` thresholds **cannot be guessed** — they must come from measurement (§6). Optionally
expose one master "classifier on/off" so you can disable it and fall back to the current
always-latch behaviour instantly if it misbehaves.

---

## 6. Tuning procedure (do this BEFORE trusting thresholds)

1. **Instrument:** temporarily have the firmware log all four metrics (`reversals`, `jerk`,
   `dom_ratio`, `directionality`) plus `movement_rms` on every classification window, with a
   timestamp. No decision yet — just log.
2. **Capture "open" samples:** trigger several **open and close** cycles (via remote AND via
   HA) with no wind. Record the metric values. These are your *smooth* cluster.
3. **Capture "wind" samples:** on a genuinely windy day (or simulate by hand-flicking/shaking
   the bar irregularly), record the metrics. These are your *chaotic* cluster.
4. **Pick thresholds** that separate the two clusters with margin — set each `*_MAX` above the
   worst "open" value and below the best "wind" value. If the clusters overlap on one metric,
   lean on another (that's why we compute several).
5. **Validate:** re-run opens and a windy period with the decision enabled; confirm opens stay
   at occupancy 0 and wind latches 1. Adjust.
6. Record the chosen numbers here.

---

## 7. Edge cases & honest limits

- **Smooth strong steady gust** could look like a move → the `MOVE_MAX_S` bound catches it if it
  persists; a brief smooth gust that stops in <45 s could be missed. Rare for real gusty wind.
- **Stuttering/segmented motor** (or a remote press that jogs the awning in steps) could look
  chaotic → false occupancy 1 during a manual jog. Harmless-ish (clears after 30 s quiet).
- **Move immediately followed by wind** — the grace window might suppress the first seconds of
  real wind. Bounded by `MOVE_MAX_S` and by wind quickly looking chaotic.
- **No classifier is 100 %.** Target is "reject the everyday smooth open/close," not perfection.

### Second layer (recommended, cheap): HA suppression on HA-commanded moves

HA can *send* open/close (even though it can't see the remote). Independent of this firmware:
when HA issues an open/close, have HA **ignore the occupancy sensor for ~45 s**. That perfectly
handles every HA-commanded move; the firmware classifier then only has to cover **remote-driven**
moves. Belt-and-braces — the two layers cover each other's blind spots. (HA-only can't work
alone because it's blind to the remote; firmware-only carries all the ambiguity. Together they're
strong.)

---

## 8. Cost

- Runs a ~1.5 s window + some float math **only on a motion event**, not while idle — so idle
  battery is unaffected; a small awake-cost per motion. Negligible vs. the value.
- More code than the current pure latch; all confined to the report task + a new
  `adxl345_classify_window()`-style helper in the driver (reuses the existing sampling loop).

---

## 9. Decision checklist before implementing

- [ ] Confirm the window length (1500 ms) feels acceptable as occupancy-reaction latency.
- [ ] Run §6 tuning; fill in the `*_TBD*` thresholds with measured values.
- [ ] Decide whether to also add the HA second layer (§7).
- [ ] Decide on a master on/off switch to fall back to always-latch.
- [ ] Then: implement, test opens (remote + HA) and a windy period, record results.
