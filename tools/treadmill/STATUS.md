# Implementation status

Current as of 2026-10-17. Details and history: [HANDOFF.md](../../docs/treadmill/HANDOFF.md); the old per-checkpoint log is in
[docs/treadmill/archive/](../../docs/treadmill/archive/STATUS-implementation-log-20261016.md) and is historical.

## What works (hardware-confirmed on the RunPad W1)

- **Native Linux backend** (BlueZ helper process, `treadmill-linux-helper`) runs our own workouts end to end; Windows/WSL remains available as a bridge (walking speeds only, 70-minute cap).
- **Walking mode** (handrail down): 1.0-6.0 km/h, incline levels 0-8; 60-minute sessions completed.
- **Running mode** (handrail up, `--running-mode`): up to 12.0 km/h, validated in stages at 7, 8, 10 and 12 km/h with an operator display check; a 40-minute running session (to 8.5 km/h) completed cleanly.
- **Wake handling:** `--wait-wake` (auto-start after a short grace, or `--wait-wake-confirm` to start on ENTER). The console sleeps after about 10 minutes idle and no keep-awake method is known.
- **Safety:** own motor gate (profile limits, stale-status watchdog 1.5 s, one STOP, emergency STOP on EOF/fault, FTMS consistency guard in running mode).
- **Garmin FIT:** workouts built from JSON specs (`trainings/`, 53 trainings, `workouts/*.fit`), plans (`plans/`), activity export; checked with Garmin's official SDK decoder.

## Not yet proven

- Incline percent per level (1.375 %/level is provisional) and true belt speed under load.
- Physical arrival and belt stop are observed through status and the operator, not measured independently.
- Garmin watch and Garmin Connect import (decoder-clean only).
- Sessions over about 62 minutes, thermal behavior, and most trainings (only a few have run on the hardware).
- Keep-awake: BLE polling, READY, A2DP/AVRCP and FTMS did not prevent the console sleep; the bundled remote is the open lead.

## Build, test, CI

`treadmill.pro` (CLI), `linux/linux-helper.pro`, `fit/treadmill-fit.pro`; offline tests under `tests/` (fast ones directly, the real-time fake-peer tests through
`tests/run_parallel.py`). CI: `.github/workflows/treadmill.yml` (Linux offline, staged install, clean Debian package build, sanitizers; Windows compile and offline gates).

See [README.md](README.md), [ENGINEERING.md](ENGINEERING.md), [API.md](API.md) and [docs/treadmill/VO2MAX.md](../../docs/treadmill/VO2MAX.md).
