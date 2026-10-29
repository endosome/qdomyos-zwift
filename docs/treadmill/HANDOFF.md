# RunPad W1 treadmill project handoff

Current state as of 2026-10-28 (TUI and the two-ENTER approval added; see "Terminal UI"). The full chronological log (every trial, dead end and checkpoint, about 1,400 lines) is
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
- **TUI:** `tools/treadmill/treadmill_tui.py` (curses) wraps `workout run --manual-control`: training picker, the CLI's approval prompts, live run screen with next-segment heads-up, link age, console pause and safety-key banners, run summary and FIT export. Fake-treadmill tested only.
- **Tests and CI:** C++ gtests (gate), Python suites under `tools/treadmill/tests/` (fake-peer trials run through `run_parallel.py`, about 3 minutes), Docker clean-package build, `.github/workflows/treadmill.yml` (green on `develop`).

## Terminal UI (2026-10-28)

Launch it on the Pi, in a terminal of at least 80x24 (120x36 shows the event log too):

```sh
treadmill-tui
```

`~/.local/bin/treadmill-tui` is a symlink to `~/qdomyos-zwift/tools/treadmill/treadmill_tui.py` (executable); `python3 <that file>`
works too. `?` opens the key help on every screen (other keys keep working while it is open, so `q` still stops the run).

- It runs the installed `treadmill` on PATH (`~/.local/bin/treadmill`, reinstalled on 2026-10-28 with the two-ENTER prompt).
  A CLI built before that asks for `MOTOR` and the TUI shows "THIS CLI IS TOO OLD": rebuild and `make install` (README, Build).
  `TREADMILL_BIN=<path>` selects another CLI, `TREADMILL_LOG_DIR=<dir>` another log directory (default `~/treadmill-logs`).
  Extra plan files can be passed as arguments (`treadmill_tui.py my-plan.xml`); they appear under "Files".
- Flow: picker (←→ category, ↑↓ training, `/` search, `h` handrail up = `--running-mode`, `w` wait for wake, `m` manual strategy),
  ENTER → the CLI's safety text and its two ENTER prompts → "wake the console" → ENTER starts the belt → run screen → summary
  (`f` exports a FIT activity next to the log). Run keys: `q`/`s`/Ctrl-C stop (Esc deliberately does not), ↑↓ speed ±0.1
  (PgUp/PgDn ±0.5), ←→ incline level, `r` back to the plan, `space` pause, `b` terminal bell (off by default; `treadmill-tui --bell` starts with it on).
- The command it starts: `treadmill workout run <id> --confirm-motor --log ~/treadmill-logs/<id>-<timestamp>.jsonl --manual-control
  --manual-strategy hold [--running-mode] [--wait-wake 10 --wait-wake-confirm]`, on a pty, reading the JSON records on stdout
  (API.md, "Manual control"). If the TUI exits, the CLI's input closes and it stops the belt.
- Design: `Child` (pty + reader threads; stdout is always drained), `StderrParser` (drops the plan JSON, counts the prompts),
  `RunState` (everything on screen, built only from records), then curses drawing in `App`. The safety key is
  `raw_status` 06/01 (06/02 is sleep); distance is the FTMS counter when present (running mode), otherwise speed integrated.
- Choices not yet confirmed on hardware: it always uses `--manual-control` (the helper then accepts any target inside the
  mode's limits, not only the plan's sequence; never run on the W1, and never together with a console pause), default
  strategy `hold` (the CLI's default is `takeover`).
- Pause from the TUI is a placeholder: it sends `pause`/`resume`, which the CLI refuses ("unknown command"); the TUI then
  points to the console's Pause/Play. Contract in API.md ("Planned"), backlog item below.
- Tests: `tests/test_tui.py` (model, no binary, in CI) and `test_the_tui_driver_...` in `test_motortrial.py` (real CLI, fake
  peer). Checked by hand in tmux against the fake helper extracted from `test_motortrial.py` (set `TREADMILL_LINUX_HELPER`,
  `MOTOR_CALLS`, `MOTOR_MODE=normal`, `MOTOR_BACKEND=linux-bluez`, optionally `MOTOR_WAKE_AFTER`, `MOTOR_CONSOLE_PAUSE`).
  Use a private tmux server (`tmux -L test -f /dev/null`); the user's tmux config splits new windows.
- First hardware session: a short walking training (e.g. `hike-recovery-35`), handrail down, operator at the console stop;
  watch the link indicator, try ↑↓ once, a console Pause/Play, then `q`.

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
- The FitShow pause command does not work. A console Pause shows up as STOPPING (4) with falling speed, then NORMAL (0), the same as a stop.
  Linux workouts treat that as a pause and continue after Play (`console_pause`/`console_play`/`console_resumed`), returning gradually to the plan.
  This is not hardware-proven yet: what Play reports (countdown? the restart speed and incline?) is still to be captured.
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
| `hike-recovery-35` started in both modes (handrail down; handrail up with `--running-mode`) | belt started and reached the first target (2.4 km/h) in each; both were stopped by the operator after about 15 s, so neither is a full run |
| AVRCP volume | does not silence the console beeps |

## Not proven / open

- Incline percent per level and true belt speed under load; physical arrival and belt stop are not independently measured.
- Garmin watch and Garmin Connect import (only decoder-clean); activity export is verified with Garmin's decoder.
- Sessions longer than about 62 minutes (the 90-100 minute trainings are marked UNPROVEN LENGTH and kept out of plans); the console's own auto-stop is untested.
- Thermal behavior is unobservable (no temperature field exists in the protocol).
- Keep-awake: BLE polling, READY, AVRCP/A2DP connect and FTMS did not help. Open leads: identify the bundled remote (IR, 433 MHz or BLE; `btmon` while pressing it), an audio-stream test, a held FTMS Request Control session, a mechanical button pusher.
- Console Pause/Play (commit `7c963b2b9`) is tested against the fake console only. The 2026-10-26 hardware attempts never got as far as the pause; see the next item. Test plan:
  - Session A, handrail down: `workout run hike-base-60 --wait-wake 10 --wait-wake-confirm > ~/pause-a.out 2>&1`. After minute 7 (3.8 km/h, level 4), press Pause, wait about 30 s and note whether the deck lowers, then press Play while standing on the side rails.
  - Session B, handrail up: `run-easy-aerobic-40 --running-mode`, with the same redirect. Pause at 7.0 km/h (minute 4-5), and optionally again at 8.5 km/h.
  - Check each log with `summarize_run.py`. Adjust the code if Play restarts at full speed, if state 1 or 10 appears, or if the deck behaves differently from the fake.
- Bluetooth stalls stop runs ("Missing status or start deadline"; the helper reports "Linux motor watchdog requested stop"). They were seen on 2026-10-18 and three times on 2026-10-26: logs `hike-base-60-20261026-151033` (stall at +2:31), `-161022` (5.3 s during the countdown) and `-162208` (a poll write unanswered for 11.6 s during the countdown).
  - Likely cause on 2026-10-26: the Pi itself was overloaded. Another session was running `oura redecode`, writing about 25 MB/s, with 20-48 % iowait, load 7-8 on 4 cores and 1.7 of 2 GB swap in use. The two countdown stalls fell inside that job. The +2:31 stall coincided with the per-minute `wifi-reliability.service` (Wi-Fi and Bluetooth share the Broadcom chip) and with Oura timer activity; that link is unproven.
  - Before a run, check that the Pi is idle (`top`: low `wa`, little swap traffic). Optionally stop `wifi-reliability.timer` for the run and restart it afterwards.
  - Safety gap: when a GATT write hangs, the helper's emergency STOP queues behind it. In `-162208` the STOP went out 12 s late. In `-161022` the STOP was sent during the countdown, yet the console still started the belt at 1.0 km/h and kept it running with no host attached. The operator's console Stop is the real safeguard.
  - `summarize_run.py` ignores the silence after the last status, so "longest gap" understated `-162208` (it printed 0.58 s).
- Backlog: pause and resume from the host. Our only FitShow pause trial (`5306`) did nothing on the W1, but the FitShow mobile app
  has a pause, so the W1 probably accepts some other sequence. Capture the app's traffic while pausing and resuming (Android
  HCI snoop log or `btmon`), then add the `pause`/`resume` manual commands specified in `tools/treadmill/API.md`
  ("Planned"). `treadmill_tui.py` already sends them and currently shows the CLI's refusal.
- Walking trainings also run with the handrail up: pass `--running-mode` (the profile is fixed before connecting, so the CLI does not auto-detect the handrail). With the handrail up and no flag, the CLI faults before the belt moves.
- Most of the 54 trainings (including every VO2 training except `vo2-30-30-light-30`, and the 12 km/h mountain/trail ones) have never run on the hardware; the new VO2 trainings' paces are starting points.

## Working rules

- Motor runs need the operator at the console with the belt clear and a physical stop within reach; the CLI asks for ENTER, then ENTER again (until 2026-10-28 it asked for a typed `MOTOR`). Do not launch runs unattended, and ask the operator before waking the console or starting a stage.
- Drivers (scripts) must write the CLI's output to files. Prefer `workout run <id> --wait-wake 10 --wait-wake-confirm` (add `--running-mode` for running trainings).
- Walking trainings need the handrail down; running trainings need it up and the staged validation done.
- Commit after each significant change. `develop` was rebased on `master` and force-pushed with a lease; `master` tracks `cagnulein/qdomyos-zwift`.
- Do not commit `tools/treadmill/__pycache__/`.

## Suggested next work

1. Run the console Pause/Play hardware test (sessions A and B above) on an idle Pi, and a first supervised TUI session
   ("Terminal UI" above).
2. Make the emergency STOP independent of a hung write: for example, cancel or skip the pending write, or disconnect if STOP is not acknowledged within about 2 s. Find out why a STOP during the countdown does not prevent the belt start. Count the trailing silence in `summarize_run.py`.
3. Run the remaining trainings on the hardware, one new feature per supervised run (skimo and hiking first, then the running and VO2 sets); a 90-minute run before relying on anything over 65 minutes.
4. Calibrate: inclinometer reading per level and belt-speed revolution timing (VO2MAX.md), then replace the provisional 1.375 %/level.
5. Identify the bundled remote and test it as a wake or keep-awake method.
6. Load an exported FIT workout into a Garmin watch and Garmin Connect.
7. Optional: an adaptive stop for ramp tests; a Windows CI job run on a real runner (the Windows job passes on CI today).
