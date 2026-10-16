# Trainings and plans for ski touring, hiking and mountain running

This directory holds the **training library** for the RunPad W1: 26 trainings, each a JSON
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

- **Walking mode is 1.0-6.0 km/h.** In it, "mountain running uphill" and "trail" sessions are **power-hiking**: fast,
  steep, upright walking, which is also how steep mountain races are climbed (the `mr-` and `trail-` trainings).
- **Running mode (up to 12 km/h) exists but is locked until validated.** The console has a separate running mode, and the
  `mrun-` and `trail-run-` trainings use it. No motor trial has yet shown that the raw speed scale in running mode
  matches the console, so the CLI unlocks it in stages (see "Running mode" below). Running form and downhill running
  still cannot be trained: the belt only goes up.
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
- **Session length.** Most sessions are 35-65 minutes; three 90-100 minute sessions are marked **unproven length** and kept out of the plans. The tool and the Linux backend allow up to 120
  minutes (the Windows bridge stays at 70). The longest continuous run on this machine so far is 62 minutes, so
  anything longer is unproven: the console may stop on its own at a fixed time, and motor temperature cannot be
  read from the treadmill (the protocol has no such field). Check the motor housing by hand on long sessions.
- **This is general conditioning guidance, not medical advice and not individualised coaching.** Check with
  a doctor before starting if you have any health concern, stop if you feel unwell, and keep the safety key
  attached and the stop within reach.

## The library

<!-- catalog:start -->
| Training | Sport | Minutes | Vertical (est.) | Top speed | Top level | Effort |
|---|---|---|---|---|---|---|
| `hike-base-60` | hiking | 60 | ~178 m | 3.8 km/h | 4 | easy/steady |
| `hike-long-day-100` | hiking | 100 | ~316 m | 3.6 km/h | 5 | easy/steady |
| `hike-recovery-35` | hiking | 35 | ~24 m | 3.2 km/h | 1 | easy/steady |
| `hike-rolling-hills-60` | hiking | 60 | ~143 m | 3.8 km/h | 5 | easy/steady |
| `hike-steep-intervals-50` | hiking | 50 | ~132 m | 3.4 km/h | 7 | hard |
| `hike-steep-pack-65` | hiking | 65 | ~230 m | 3.4 km/h | 6 | easy/steady |
| `mr-hill-repeats-8x2` | hiking | 50 | ~210 m | 6.0 km/h | 8 | hard |
| `mr-powerhike-5x5` | hiking | 55 | ~225 m | 5.6 km/h | 6 | hard |
| `mr-strength-endurance-60` | hiking | 60 | ~288 m | 5.0 km/h | 6 | easy/steady |
| `mr-vk-progression-60` | hiking | 60 | ~342 m | 5.8 km/h | 8 | hard |
| `mrun-hill-repeats-6x3` | running | 48 | ~258 m | 9.0 km/h | 6 | hard |
| `mrun-uphill-tempo-50` | running | 50 | ~243 m | 8.0 km/h | 4 | hard |
| `mrun-vertical-intervals-55` | running | 55 | ~310 m | 8.0 km/h | 8 | hard |
| `skimo-base-60` | hiking | 60 | ~307 m | 4.2 km/h | 7 | easy/steady |
| `skimo-kickturn-terrain-55` | hiking | 55 | ~253 m | 4.8 km/h | 8 | hard |
| `skimo-long-climb-65` | hiking | 65 | ~312 m | 4.4 km/h | 7 | easy/steady |
| `skimo-long-day-100` | hiking | 100 | ~552 m | 4.4 km/h | 7 | easy/steady |
| `skimo-threshold-4x8` | hiking | 60 | ~329 m | 5.0 km/h | 8 | hard |
| `skimo-vam-ladder-60` | hiking | 60 | ~372 m | 5.8 km/h | 8 | hard |
| `skimo-vertical-race-prep-50` | hiking | 50 | ~246 m | 5.8 km/h | 8 | hard |
| `trail-long-rolling-65` | hiking | 65 | ~156 m | 5.2 km/h | 4 | easy/steady |
| `trail-rolling-terrain-60` | hiking | 60 | ~172 m | 5.6 km/h | 7 | easy/steady |
| `trail-run-long-90` | running | 90 | ~326 m | 8.0 km/h | 4 | easy/steady |
| `trail-run-progression-45` | running | 45 | ~122 m | 10.0 km/h | 2 | easy/steady |
| `trail-run-rolling-60` | running | 60 | ~273 m | 9.5 km/h | 7 | easy/steady |
| `trail-tempo-progression-55` | hiking | 55 | ~175 m | 6.0 km/h | 4 | hard |
<!-- catalog:end -->

"Effort": a training is **hard** when it contains interval-intensity steps; the plans never put hard
sessions on consecutive days or more than two in a week.

| Discipline | Trainings |
|---|---|
| Ski touring (`skimo-`) | steady base, long climb with transitions, threshold 4x8, vertical-rate ladder, switchbacks, race-pace prep |
| Hiking (`hike-`) | steady base, rolling hills, steep with pack, recovery walk, steep intervals |
| Mountain running uphill, walking mode (`mr-`) | power-hike 5x5, hill repeats 8x2, vertical-km progression, strength endurance |
| Trail, walking mode (`trail-`) | rolling-terrain loop, tempo progression, long rolling day |
| Running mode (`mrun-`, `trail-run-`) | hill repeats 6x3, uphill tempo, vertical intervals, rolling trail loop, progression run, long trail run |
| Long sessions, unproven (`*-long-day-100`, `trail-run-long-90`) | 90-100 minute sessions: kept out of the plans until that length has run |

## The plans

| Plan | Weeks | Shape |
|---|---|---|
| `plan-skimo-8w` | 8 | base (3) - recovery - build (2) - peak - taper; 155-220 min a week |
| `plan-hiking-6w` | 6 | base - back-to-back long days - recovery - build - taper; 155-235 min |
| `plan-mountain-run-8w` | 8 | base (3) - recovery - build (2) - peak - taper; 145-215 min (walking mode) |
| `plan-trail-runner-8w` | 8 | the same shape with real running speeds; 80-198 min (needs running mode, validated) |

`plan.py check` enforces the guard-rails on every plan: at least two rest days, at most two hard sessions a week
and never on consecutive days (including across the Sunday-Monday boundary), weekly minutes at most 20 % above
the larger of the two previous weeks, a taper at the end, and every training must exist. The plans also list
outdoor and strength sessions as plain text; those are suggestions, not prescriptions.

## Running mode (Linux backend, opt-in)

Raw speeds above 60 (6.0 km/h) exist only when the console is in its **running mode**; the W1 then reports limits of
10-120 with unit tag `0x20`. The protocol document says speeds are in 0.1 km/h units, which would make raw 100 = 10.0
km/h, but that has never been checked against this treadmill in running mode. So:

- Everything above 6.0 km/h needs `--running-mode` (the CLI adds `--observe-ftms` itself) and the console in running mode.
  The walking profile refuses the running limit report and the running profile refuses the walking one.
- **FTMS is the independent speed check.** While above 6 km/h the run is stopped, by both the CLI and the helper, if the
  speed the treadmill reports over FTMS disagrees with its own FitShow speed (more than 0.6 km/h for 4 reports) or cannot
  be observed for 2 s.
- **Staged unlock.** Four trials approach their top speed through 6.0 km/h, hold it for 15 s and come back:
  `treadmill control-test --backend linux --device DD:87:70:39:74:20 --expected-name FS-0F7A32 --running-mode --trial run-7`
  (then `run-8`, `run-10`, `run-12`, in that order, with you at the controls and the belt clear). A stage unlocks its speed
  only when it completes and FTMS agreed with FitShow for at least 5 s at the top speed; the result is recorded in
  `~/.local/state/treadmill/running-validated.json`. Workouts above the recorded speed are refused. Deleting or editing
  that file is your decision to override it.
- A stop from high speed gets a longer window for the belt to decelerate and report NORMAL three times.

## Files, formats and rebuilding

- `trainings/<id>.json` is the source. Steps are `{name, minutes|seconds, speed, level, intensity, notes}`;
  `{ramp: {...}}` expands into equal steps (1 minute by default); `{repeat: N, steps: [...]}` is one level of
  repeats. The tool refuses anything outside the W1's limits, speeds that are not on the 0.1 km/h grid, steps
  under 30 s and trainings over 70 minutes.
- `workouts/<id>.fit` is generated: run `trainings/build.sh` (or `build.sh --check`, which CI runs, to prove
  the committed files are exactly what the specs build). Building is deterministic.
- **FIT layout (planned workouts).** One FIT *workout* file per training, written with Garmin's official FIT SDK
  and verified with Garmin's own Python decoder. Each step has a time duration, a **speed** target (a +/- 0.2 km/h
  band, so a watch's own speed alert stays usable) and the incline level as a secondary **grade** target for our
  CLI. Garmin's workout builder has **no incline target** and FIT defines no scale for grade, so watches ignore
  that secondary target (we store hundredths of a percent, the scale Garmin uses for `record.grade`). What watches
  do show is each step's **notes**, which lead with the incline in plain ASCII ("Incline 8.3% (L6), 4.0 km/h, about
  330 m/h up. RPE 5."). Some watches show notes at the start of each step, others only in a menu, and some ignore
  step names, so the notes carry the information. Garmin Connect is reported to accept at most 50 steps per
  workout; the tool refuses more. Repeats are standard FIT repeat steps. Loading these files into a watch has not
  been tested.
- **Completed runs as FIT activities (what Garmin does not ignore).** `treadmill-fit activity <run.jsonl> <out.fit>`
  turns a run log into a FIT *activity*: 1 Hz records of the speed and incline the treadmill reported, distance,
  and an elevation estimate, with total ascent on the session and lap. Garmin Connect reads those and credits the
  distance and elevation gain. The altitude is **an estimate** (speed x grade, with the working 1.375 %-per-level
  table), not a measurement. The session sport is hiking, walking or running (`--sport`); its sub-sport is
  generic on purpose, because treadmill activities may have their altitude dropped. If you also record the same
  session on a watch you will have two activities in Garmin Connect; delete one.
- **Plans are not FIT.** FIT has no practical multi-week plan format, so a plan is a small JSON manifest that
  references trainings by id.
- Add a training by writing `trainings/<id>.json`, running `build.sh`, and adding it to a plan (the tests
  require every training to appear in at least one plan).
