# Touch Slider Algorithm

## 1) Core Objective
Detect intentional directional slides with high reliability while rejecting:
- tap spikes
- baseline drift
- one-side noise
- ambiguous dual-side contact

## 2) Processing Pipeline
1. Read raw values from left/right touch channels.
2. Compute raw deltas against adaptive baselines.
3. Update idle-noise estimates when no session is active.
4. Apply per-side noise compensation floor.
5. Compute contact strength:
   - total delta
   - max-side delta
6. Freeze or update baselines depending on contact state.
7. Run gesture state machine:
   - side visits and sequence timing
   - filtered balance and dominant side
   - travel/crossing checks
8. Emit usage when gesture criteria pass.
9. If enabled, schedule and execute hold-repeat.

## 3) Direction Semantics
- `R->L` emits `left_usage`
- `L->R` emits `right_usage`

Configured in `g_touch_layer_config`.

## 4) Hold-Repeat Model
- Starts only after a valid gesture fires.
- Requires hold-repeat enabled for that side.
- Continues while contact remains engaged and dominant side is maintained.
- Stops immediately when hold condition breaks.

## 5) Tuning Knobs
All tunables are in `config/keymap_config.yaml` under `touch.*` (generated as `MACRO_TOUCH_*` constants).

Main groups:
- active/release thresholds
- baseline freeze thresholds
- contact minimums
- side sequence and timing windows
- direction dominance and travel deltas
- debug logging
- idle-noise compensation

## 6) Debugging
- Set `MACRO_TOUCH_DEBUG_LOG_ENABLE` to `true`.
- Observe logs for:
  - baselines
  - raw/compensated deltas
  - balance/filter values
  - side-seen status
  - session/gesture flags
