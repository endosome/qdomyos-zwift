# VO2max testing on the W1

The treadmill supplies a known, repeatable load; the mask (for example a VO2 Master) measures the gas exchange.
Use running mode (handrail up): walking mode stops at 6.0 km/h, which is about 33 ml/kg/min even at full incline.

## Trainings

| Training | Use |
| --- | --- |
| `vo2-economy-submax-35` | four 5-minute stages (8, 9, 10, 11 km/h, level 1): steady-state VO2 per speed, running economy |
| `vo2-ramp-moderate-30` | ramp to exhaustion from 7.0 km/h: +0.5 km/h per minute to 12.0, then +1 level per minute |
| `vo2-ramp-trained-30` | the same from 9.0 km/h |
| `vo2-verify-moderate-24`, `vo2-verify-trained-24` | constant-load verification bout, 2-3 days after the ramp |

Plans: `plan-vo2max-moderate-4w`, `plan-vo2max-trained-4w` (baseline, two easy weeks, retest).
Run with `treadmill workout run <id> ... --running-mode --wait-wake 10 --wait-wake-confirm`.

## Load model (ACSM running equation, flat plus grade)

VO2 (ml/kg/min) = 0.2 * v + 0.9 * v * grade + 3.5, with v in m/min and grade as a fraction.
12 km/h flat is about 44; 12 km/h at 11 percent (level 8, provisional) is about 63.
One speed step (0.5 km/h) adds about 1.7 ml/kg/min per minute; one grade level (about 1.375 percent) at 12 km/h adds about 2.5.
The equation is a guide for choosing a ramp, not a replacement for the measured VO2. It is least accurate above about 12 km/h
and for the unverified incline here.

## Before the first test

1. Incline: set each level in turn and read the belt angle with a digital inclinometer. The 1.375 percent per level figure is provisional (docs/treadmill/HANDOFF.md).
   Use the measured percent, not the label, when you compare VO2 against load.
2. Belt speed: mark the belt, time 20 revolutions at 8 and 12 km/h with a stopwatch, and compare with the displayed speed
   (belt length / revolutions x time). The console display matched the commands within 0.1 km/h but true belt speed under load is unmeasured.
3. Check the validated running speeds cover 12.0 km/h (`~/.local/state/treadmill/running-validated.json`).
4. Mask: fit and leak-check it, and calibrate the analyzer per its manual.

## Protocol

- Rested, same time of day, no hard run the day before, fed 2-3 hours earlier, same shoes.
- Warm-up (in the schedule), then the ramp. Aim for 8-12 minutes; if the test is shorter than 8 minutes pick the easier ramp, longer than 12 pick the harder.
- Stop at exhaustion with the console STOP key. A spotter stands next to the belt: the belt decelerates within a few seconds
  from speed and the person is breathing hard through a mask. The key or clip must be within reach.
- Verification bout on a different day (2-3 days later): if its VO2 does not exceed the ramp peak by more than about 3 percent (the usual criterion), the ramp peak is VO2max.
- Time alignment: the treadmill log (`~/treadmill-logs/*.jsonl`) carries UTC timestamps of every target reported; the mask data needs a start mark at the same clock.
  `treadmill-fit activity` exports the speed and estimated altitude trace.

## Limits

- The incline has only 9 levels (about 1.4 percent each); speed has 0.1 km/h resolution.
- 12.0 km/h at level 8 is the maximum load; a very fit runner can hit that ceiling before exhaustion (then the test measures the treadmill, not the athlete).
- No downhill.
- Thermal behavior of the treadmill is unobservable; keep tests short and let the machine rest between a ramp and its verification.
- The ramps are fixed schedules: they do not adapt to the athlete.
