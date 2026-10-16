# RunPad W1 physical validation worksheet

The operator initially requested one trial and confirmation at a time, then
explicitly changed the workflow to batch all remaining trials and analyze them
together. The remaining batch is speed-50/60, incline-1 through incline-8,
then pause, with explicit stop and three NORMAL replies between trials and
immediate batch halt on any failure. Its reviewable plan is
`remaining-batch-plan.json`. Individual plans remain in `prepared-trials.json`;
list them with `treadmill control-test --backend windows --list-trials`.
Every motor launch rechecks identity, walking limits and fresh status. The
operator must keep the belt clear and remain at the physical stop throughout.

## Prepared motor trials

| Trial | Raw targets | Observation holds | Evidence needed |
|---|---|---|---|
| start-stop | speed 10, incline 0; stop | 5 s reported running | Passed 2026-10-14: physical start/stop, console 1.0/0, untouched controls |
| speed-12 | 10 → 12 → 10; incline 0 | 3/3/3 s | First in-session speed update and return, console/physical response |
| speed-15 | 10 → 15 → 10; incline 0 | 3/5/3 s | Intermediate speed and settling/deceleration |
| speed-20 | 10 → 20 → 10; incline 0 | 3/5/3 s | Console 2.0 correspondence, physical response |
| speed-30 | 10 → 30 → 10; incline 0 | 3/5/3 s | Console 3.0 correspondence, physical response |
| speed-40 | 10 → 40 → 10; incline 0 | 3/5/3 s | Console 4.0 correspondence, physical response |
| speed-50 | 10 → 50 → 10; incline 0 | 3/5/3 s | Console 5.0 correspondence, physical response |
| speed-60 | 10 → 60 → 10; incline 0 | 3/5/3 s | Reported walking cap, physical response; requires its own review |
| incline-1 … incline-8 | incline 0 → named code → 0; speed 10 | 3/15/15 s | Every code, ascending/descending movement, response lag and attained position |
| pause | speed 10/incline 0; pause; stop | 3 s running + 5 s pause observation | Belt deceleration/stop, display/telemetry state, retained workout state |

Baseline and speed-12/15/20/30/40 have been confirmed physically by the operator.
Speed-40's software observer ended at five seconds with two NORMAL samples;
explicit stop and cleanup succeeded, and the operator confirmed physical stop.
The observer now allows ten seconds. Detailed confirmations and captures are
recorded in `validation-results.json`. No percentage-grade measurements exist.

Each hold starts at matching reported feedback; mismatches restart the hold.
No target retries, automatic restart or general raw-write facility is allowed.
Incline holds are observation windows, not proof that the deck has attained
position. Speed values are raw tenths under the operator's walking-mode
assumption; only raw 10 ↔ console 1.0 has been confirmed so far. No independent
belt-speed measurement or percentage-grade calibration exists.

For each trial record: console speed/incline at each stage, actual belt/deck
behavior, whether return to baseline completed, lag in both directions,
physical stopping, any physical intervention, the trace path and discrepancies.
If a trial fails, resolve that named gap before progressing to larger targets.

## Additional checks required for understanding and release

These need reviewed, separate procedures after the relevant controls pass;
they are not automatically authorized or inferred from the prepared suite.

| Check | Required evidence / dependency |
|---|---|
| Repeat low-speed start/stop | Several cold/warm cycles, ownership and sleep/wake behavior; no blind retries |
| Stop while countdown is active | Physical cancellation before movement, status and countdown transition |
| Stop from a pending target | Priority stop actually overtakes a target; no later target/restart |
| Stop idempotence | A second explicit stop on a physically stopped belt; do not infer from the first stop |
| Pause/resume | Observe pause first, review actual state and required resume sequence before adding resume |
| Q/S, EOF, termination signal | Physical low-speed stopping and logs on each operator interruption path |
| Parent-process loss / stale polling | Windows watchdog physically stops at low speed; software tests alone do not establish this |
| Bluetooth loss | Observe actual device behavior with operator at controls; no deliverable stop over a lost link |
| Alternate mode | Resolve raw 60/tag 0 versus 120/tag 0x20, document console mode and limits; present plans refuse alternate bounds |
| Firmware/initialization | Firmware remains unknown; successful minimal sequence establishes no initialization was needed for that session only |
| Speed calibration | Explicit km/h confirmation plus independent approved belt measurement; multiple points, resolution, limits and lag |
| Incline calibration | Measure grade at every attained code, both directions, baseline, hysteresis and lag; do not treat codes as percent |
| Feedback semantics | Determine whether incline is target or attained position; short NORMAL has no zero-speed measurement |
| Ownership/reconnect | Single controller behavior, lost owner, fresh identity/bounds; no automatic restart |
| Short real workout | Only after mappings, stop/fault evidence and timing policy pass; full 20-minute workout is later |

No radio sniffer or phone capture is currently needed for the successful
minimal control path. Official-app traces become useful if initialization,
pause/resume or mode behavior cannot be determined from local commands/replies.

## First completed trial

`captures/windows-motor-trial-20261014-01.jsonl`: one start, target raw 10/0,
5.014 seconds of reported running, explicit stop and reply, STOP speeds
9/7/4/1, three NORMAL reports and successful notification cleanup. The operator
confirmed physical start/stop, console 1.0/incline 0 and untouched controls.
No raw zero or independent speed/grade measurement was recorded.
