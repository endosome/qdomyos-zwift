# Trainings and plans for ski touring, hiking and mountain running

This directory holds the **training library** for the RunPad W1: 18 trainings, each a JSON
spec (the source) that `treadmill-fit` compiles into a **Garmin FIT workout** in
`../workouts/`. Three multi-week **plans** in `../plans/` combine them. The CLI plays the
`.fit` files directly:

```sh
treadmill workout list
treadmill workout run skimo-threshold-4x8 --wait-wake 10 --wait-wake-confirm
python3 tools/treadmill/plan.py show tools/treadmill/plans/plan-skimo-8w.json --week 3
```

## What the W1 can and cannot train

Read this before you rely on any of it.

- **Speed is walking mode only: 1.0-6.0 km/h.** "Mountain running uphill" and "trail" sessions are
  therefore **power-hiking**: fast, steep, upright walking, which is also how steep mountain races are
  climbed. Running form, running economy and downhill running cannot be trained here.
- **Incline tops out near 11 %** under the provisional level mapping (level 0 = 0 %, level 8 = 11 %, 1.375 %
  per level; the W1's own telemetry reports 8 % at level 8, so treat grade as approximate). Ski-touring
  slopes are steeper, so the sessions train the *effort* (vertical rate, in metres per hour) rather than the
  slope. At the machine's best (6 km/h at level 8) the estimated vertical rate is about 660 m/h; the steady
  ski-touring sessions use roughly 300-550 m/h.
- **No downhill, no snow, no terrain.** Eccentric (downhill) strength, ski technique and skinning on snow
  need outdoor time. A 60-minute session here adds roughly 150-370 m of estimated vertical; real tours are
  several times that. The plans are a supplement, not a replacement.
- **Vertical figures are estimates**: speed x grade x time using the working grade table. They are not
  measured and not calibrated.
- **Effort is given as RPE** (1-10 perceived exertion) and the talk test, because the treadmill does not
  know your heart rate. Pick speeds you can hold; every step is a number you can override at the console.
- **Sessions are capped at 65 minutes.** The longest continuous run so far on this machine is 62 minutes;
  motor temperature cannot be read from the treadmill (the protocol has no such field), so check the motor
  housing by hand on long sessions.
- **This is general conditioning guidance, not medical advice and not individualised coaching.** Check with
  a doctor before starting if you have any health concern, stop if you feel unwell, and keep the safety key
  attached and the stop within reach.

## The library

<!-- catalog:start -->
| Training | Minutes | Vertical (est.) | Top speed | Top level | Effort |
|---|---|---|---|---|---|
| `hike-base-60` | 60 | ~178 m | 3.8 km/h | 4 | easy/steady |
| `hike-recovery-35` | 35 | ~24 m | 3.2 km/h | 1 | easy/steady |
| `hike-rolling-hills-60` | 60 | ~143 m | 3.8 km/h | 5 | easy/steady |
| `hike-steep-intervals-50` | 50 | ~132 m | 3.4 km/h | 7 | hard |
| `hike-steep-pack-65` | 65 | ~230 m | 3.4 km/h | 6 | easy/steady |
| `mr-hill-repeats-8x2` | 50 | ~210 m | 6.0 km/h | 8 | hard |
| `mr-powerhike-5x5` | 55 | ~225 m | 5.6 km/h | 6 | hard |
| `mr-strength-endurance-60` | 60 | ~288 m | 5.0 km/h | 6 | easy/steady |
| `mr-vk-progression-60` | 60 | ~342 m | 5.8 km/h | 8 | hard |
| `skimo-base-60` | 60 | ~307 m | 4.2 km/h | 7 | easy/steady |
| `skimo-kickturn-terrain-55` | 55 | ~253 m | 4.8 km/h | 8 | hard |
| `skimo-long-climb-65` | 65 | ~312 m | 4.4 km/h | 7 | easy/steady |
| `skimo-threshold-4x8` | 60 | ~329 m | 5.0 km/h | 8 | hard |
| `skimo-vam-ladder-60` | 60 | ~372 m | 5.8 km/h | 8 | hard |
| `skimo-vertical-race-prep-50` | 50 | ~246 m | 5.8 km/h | 8 | hard |
| `trail-long-rolling-65` | 65 | ~156 m | 5.2 km/h | 4 | easy/steady |
| `trail-rolling-terrain-60` | 60 | ~172 m | 5.6 km/h | 7 | easy/steady |
| `trail-tempo-progression-55` | 55 | ~175 m | 6.0 km/h | 4 | hard |
<!-- catalog:end -->

"Effort": a training is **hard** when it contains interval-intensity steps; the plans never put hard
sessions on consecutive days or more than two in a week.

| Discipline | Trainings |
|---|---|
| Ski touring (`skimo-`) | steady base, long climb with transitions, threshold 4x8, vertical-rate ladder, switchbacks, race-pace prep |
| Hiking (`hike-`) | steady base, rolling hills, steep with pack, recovery walk, steep intervals |
| Mountain running uphill (`mr-`) | power-hike 5x5, hill repeats 8x2, vertical-km progression, strength endurance |
| Trail (`trail-`) | rolling-terrain loop, tempo progression, long rolling day |

## The plans

| Plan | Weeks | Shape |
|---|---|---|
| `plan-skimo-8w` | 8 | base (3) - recovery - build (2) - peak - taper; 155-220 min a week |
| `plan-hiking-6w` | 6 | base - back-to-back long days - recovery - build - taper; 155-235 min |
| `plan-mountain-run-8w` | 8 | base (3) - recovery - build (2) - peak - taper; 145-210 min |

`plan.py check` enforces the guard-rails on every plan: at least two rest days, at most two hard sessions a week
and never on consecutive days (including across the Sunday-Monday boundary), weekly minutes at most 20 % above
the larger of the two previous weeks, a taper at the end, and every training must exist. The plans also list
outdoor and strength sessions as plain text; those are suggestions, not prescriptions.

## Files, formats and rebuilding

- `trainings/<id>.json` is the source. Steps are `{name, minutes|seconds, speed, level, intensity, notes}`;
  `{ramp: {...}}` expands into equal steps (1 minute by default); `{repeat: N, steps: [...]}` is one level of
  repeats. The tool refuses anything outside the W1's limits, speeds that are not on the 0.1 km/h grid, steps
  under 30 s and trainings over 70 minutes.
- `workouts/<id>.fit` is generated: run `trainings/build.sh` (or `build.sh --check`, which CI runs, to prove
  the committed files are exactly what the specs build). Building is deterministic.
- **FIT layout.** One FIT *workout* file per training, written with Garmin's official FIT SDK and verified
  with Garmin's own Python decoder. Each step has a time duration, a **speed** primary target (a standard
  custom speed range, m/s) and the incline as a secondary **grade** target. FIT defines no scale for grade, so
  we store hundredths of a percent (the scale Garmin uses for `record.grade`) from the level mapping above;
  Garmin devices ignore it, and each step's notes spell it out ("L6 | 8.3% | 4.0 km/h | ~330 m/h"). Repeats are
  standard FIT repeat steps. Whether a given Garmin watch accepts these files has not been tested.
- **Plans are not FIT.** FIT has no practical multi-week plan format, so a plan is a small JSON manifest that
  references trainings by id.
- Add a training by writing `trainings/<id>.json`, running `build.sh`, and adding it to a plan (the tests
  require every training to appear in at least one plan).
