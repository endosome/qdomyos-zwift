# RunPad W1 treadmill project handoff

Current state as of 2026-10-17. The full chronological log (every trial, dead end and checkpoint, about 1,400 lines) is
[archive/HANDOFF-history-20261017.md](archive/HANDOFF-history-20261017.md); read it only to find out why something is the way it is.
User guide: `tools/treadmill/README.md`. Engineering notes: `tools/treadmill/ENGINEERING.md`. Short status: `tools/treadmill/STATUS.md`.
VO2max testing: [VO2MAX.md](VO2MAX.md).

## Goal

Run our own training plans (hiking, ski-touring, mountain and trail running, VO2max work) on the RunPad W1 from a Linux machine, with the
workout and every control operated locally: connect, start, set speed and incline, watch live status, handle timing and aborts. Trainings are
Garmin FIT workouts built from JSON specs; plans are JSON manifests; runs can be exported as FIT activities.

## What exists (all on `develop`)

- `tools/treadmill/treadmill` (CLI): `workout run|list|show`, `control-test` (staged trials), `scan`, `info`, `status`.
- **Linux backend:** a separate Qt/BlueZ helper process (`treadmill-linux-helper`, line protocol) and the host's own motor gate. The Windows/WSL PowerShell bridge still exists (walking speeds, 70-minute cap) but Linux is the supported path.
- **Safety:** gate profiles (walking: raw speed up to 60, tag 0; running: up to 120, tag 0, workouts only), stale-status watchdog 1.5 s, one STOP, three NORMAL replies before start and after stop, emergency STOP on EOF or fault, helper output-overflow guard, deadline `+3 s per step`, 120-minute cap (Linux).
- **Wake handling:** `--wait-wake N` (auto-start 5 s after the wake unless Q) and `--wait-wake --wait-wake-confirm` (start on ENTER only).
- **Running mode:** `--running-mode`, staged validation trials `run-7/8/10/12`, registry `~/.local/state/treadmill/running-validated.json`.
- **FIT:** `treadmill-fit build|dump|check|activity` (official SDK in `src/fit-sdk`), `trainings/*.json` (54 specs) and `workouts/*.fit`, `plan.py`, `make_plan.py`, `summarize_run.py`, plans in `tools/treadmill/plans/` (10 plans), audio probe script.
- **Tests and CI:** C++ gtests (gate), Python suites under `tools/treadmill/tests/` (fake-peer trials run through `run_parallel.py`, about 3 minutes), Docker clean-package build, `.github/workflows/treadmill.yml` (green on `develop`).

## Hardware and identity

| Field | Value |
|---|---|
| Treadmill | RunPad W1 |
| Advertised name / address | `FS-0F7A32`, `DD:87:70:39:74:20` (public) |
| FitShow module | `FITSHOW` `FS-BT-C1`, hardware 1.0, software 1.3.8 (firmware revision unknown) |
| Services | FitShow `FFF0` (`FFF1` notify, `FFF2` write-without-response), FTMS `1826` (passive observation only; control not used) |
| Classic Bluetooth | `FS-0F7A32-A` at `...:1F` is the console's A2DP/AVRCP audio speaker, not a control channel |

Never treat a name or address as proof of identity without rechecking at connection time. Raw evidence is in `captures/` (some compressed).

## Protocol facts that matter

- Frames `0x02 ... xor 0x03`; status `0x51`: state 0 NORMAL, 2 countdown, 3 running, 4 decelerating, 5 fault, 6 disabled (06/01 key, 06/02 sleep).
- Speed raw unit 0.1 km/h (confirmed on the display up to 12.0). Incline levels 0-8; the percent per level is provisional (1.375 %/level, 0-11 %), not measured.
- **Handrail selects the mode:** down = walking (max 6.0 km/h, limit report 10-60 tag 0), up = running (max 12.0 km/h, limit report 10-120 tag 0). Switching is physical.
- FTMS speed equals FitShow raw/10 exactly (same module), so it is a consistency guard only, not independent proof; running speeds are unlocked by an operator display check.
- Pause does not work in the verified path; it is unavailable.
- The console sleeps after about 10 minutes idle (06/02) and no remote method keeps it awake or wakes it.
- A host that stops reading the CLI's output makes the helper stop the belt (fail-safe): drivers must write output to files, never to unread pipes.

## Hardware-proven

| What | Result |
|---|---|
| Walking start/stop, speeds 1.0-6.0, incline 0-8 | confirmed with physical observation |
| 60-minute walking run (`uphill-walk-60`) | 99/99 targets, 62 min 39 s, clean |
| First `.fit` training (`hike-recovery-35`) | 35 min, clean |
| Wait-for-wake, confirm variant and unattended variant | verified (unattended once, with a near-simultaneous Play click) |
| Running stages 7, 8, 10, 12 km/h | validated; running speeds unlocked to 12.0 |
| `run-easy-aerobic-40` (running mode, up to 8.5 km/h) | 40 min 24 s, 11/11 targets, clean |
| `vo2-30-30-light-30` (running mode, 10 x 30 s at 10.5 km/h) | all 10 reps, 31/38 targets; belt began stopping by itself 3 s into the cooldown (cause unconfirmed), clean stop |
| AVRCP volume | does not silence the console beeps |

## Not proven / open

- Incline percent per level and true belt speed under load; physical arrival and belt stop are not independently measured.
- Garmin watch and Garmin Connect import (only decoder-clean); activity export is verified with Garmin's decoder.
- Sessions longer than about 62 minutes (the 90-100 minute trainings are marked UNPROVEN LENGTH and kept out of plans); the console's own auto-stop is untested.
- Thermal behavior is unobservable (no temperature field exists in the protocol).
- Keep-awake: BLE polling, READY, AVRCP/A2DP connect and FTMS did not help. Open leads: identify the bundled remote (IR, 433 MHz or BLE; `btmon` while pressing it), an audio-stream test, a held FTMS Request Control session, a mechanical button pusher.
- Most of the 54 trainings (including every VO2 training except `vo2-30-30-light-30`, and the 12 km/h mountain/trail ones) have never run on the hardware; the new VO2 trainings' paces are starting points.

## Working rules

- Motor runs need the operator at the console with the belt clear and a physical stop within reach; the CLI asks for `MOTOR`. Do not launch runs unattended, and ask the operator before waking the console or starting a stage.
- Drivers (scripts) must write the CLI's output to files. Prefer `workout run <id> --wait-wake 10 --wait-wake-confirm` (add `--running-mode` for running trainings).
- Walking trainings need the handrail down; running trainings need it up and the staged validation done.
- Commit after each significant change. `develop` was rebased on `master` and force-pushed with a lease; `master` tracks `cagnulein/qdomyos-zwift`.
- Do not commit `tools/treadmill/__pycache__/`.

## Suggested next work

1. Run the remaining trainings on the hardware, one new feature per supervised run (skimo and hiking first, then the running and VO2 sets); a 90-minute run before relying on anything over 65 minutes.
2. Calibrate: inclinometer reading per level and belt-speed revolution timing (VO2MAX.md), then replace the provisional 1.375 %/level.
3. Identify the bundled remote and test it as a wake or keep-awake method.
4. Load an exported FIT workout into a Garmin watch and Garmin Connect.
5. Optional: an adaptive stop for ramp tests; a Windows CI job run on a real runner (the Windows job passes on CI today).
