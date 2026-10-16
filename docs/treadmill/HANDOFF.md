# RunPad W1 treadmill project handoff

Updated: 2026-10-16

## Read this first: state on 2026-10-16 (moving to a Linux test machine)

**Scope reset.** The goal is to run our own XML training plans (up to 60
minutes) on the W1. Only three parts matter for that: `workout.cpp` (XML to
stages), the workout path in `motortrial.cpp` (stages to FitShow targets), and
the transport. Everything else in this file is investigation history: READY
wake, FTMS control point, inclinometer calibration, batches and profiles. The
user guide is `tools/treadmill/README.md`. The old long README is now
`tools/treadmill/ENGINEERING.md`.

**Fixed today (code on `develop`):**
- The Windows helper stopped motor sessions once it had recorded 4,096 frames
  ("Diagnostic trace limit reached"). It now keeps a rolling window of 2,048
  frames, and the full trace is streamed to the CLI log. This bug, not a queue
  overflow, ended the earlier 16:53 run: about 4 frames/s without FTMS, and
  about 8 frames/s with FTMS. Today's run with FTMS stopped at 8:37, as
  predicted. Its emergency STOP succeeded, and the operator confirmed the
  physical stop.
- Limits now allow 60-minute plans: plan ≤70 min, deadline ≤4500 s, ≤512
  distinct targets, output queue 4096 lines, and a 64 MB CLI log cap (was 8 MB,
  which a long run would hit).
- `treadmill workout run <plan>` now defaults to the W1 (Windows backend,
  DD:87:70:39:74:20 / FS-0F7A32, public address). It also writes a log to
  `~/treadmill-logs/<plan>-<timestamp>.jsonl`. Typing MOTOR is still required.
- If PowerShell fails to start, the CLI now prints an error instead of exiting
  silently.

**Hardware evidence today:** 8.5 minutes of `uphill-walk-20` with no
unexplained STOPPING. All targets from 2.0 to 3.0 km/h and incline 0–4 were
reported in order. Logs are in `captures/workout-runs-20261016/`. Earlier
`/tmp/runpad-*` logs are now in `captures/tmp-runs-20261015-16/`. **A full
20-minute run has still never completed. It is the acceptance test.**

**Why WSL interop kept breaking:** Molecule test containers from `~/roles`
are privileged and run systemd. On boot, their `systemd-binfmt` clears the
host's shared binfmt table, which deletes WSLInterop. Masking it with
`/dev/null:/etc/systemd/system/systemd-binfmt.service:ro` was tested and works.
The user chose to move to a Linux machine instead.

**Repository notes (2026-10-16):**
- `develop` was created from `master`; it did not exist before.
- Push to `endosome/qdomyos-zwift` through the SSH alias `github-endosome`
  (key `~/.ssh/id_ed25519_endosome`). The default `github.com` key
  authenticates as `antoni`, which has no write access. `origin` is now
  `git@github-endosome:endosome/qdomyos-zwift.git`. On a new machine, set up the
  same alias in `~/.ssh/config`, or use another key with write access.
- `push.followTags=true` in the git config, so the first push also sent
  upstream's existing tags (`build-999`, `version-2.19.0`, …) to the fork.
  They're harmless.
- The full 20-minute `uphill-walk-20` run has still never completed. It is the
  acceptance test.
- `tools/treadmill/__pycache__/` is untracked on purpose.

**Next on the Linux machine:** hardware workout playback is currently
**Windows bridge only**. Native Linux Qt/BlueZ supports scan, info and status
diagnostics but has no workout transport. The work is to connect
`runMotorTrial`'s line protocol (WRITE/RX/READY/IDENTITY/TX) to a Linux BLE
transport. It must keep the same gates: identity, limits, three NORMAL replies
before start, three NORMAL replies after STOP, and stop on faults or stale
status. Then run `uphill-walk-20` end to end, then a 60-minute plan.

## Primary goal

Run real, predefined workouts on the RunPad W1, with the workout and every
treadmill control operated locally by the machine running our software. The
software must connect to the treadmill, start and stop it, set speed and
incline, observe live feedback, and handle workout timing and aborts itself.
It must not depend on a remote service or a separate app to execute or control
the workout. QDomyos-Zwift's existing FitShow implementation is the protocol
reference and preferred code reuse point.

The target remains a headless CLI named `treadmill`, with reusable workout and
controller components. The initial sample workout is a 20-minute hiking and
ski-touring preparation session; workout definitions must remain independent
of the W1 and that one plan.

## Current state

- The repo contains a substantial standalone `tools/treadmill` CLI, workout
  engine, simulator, FitShow adapter, profile/evidence gates, and Windows
  diagnostics helper. Offline implementation and software checks are recorded
  in [STATUS.md](../../tools/treadmill/STATUS.md).
- The CLI target is `tools/treadmill/treadmill.pro`. A Linux binary exists at
  `/tmp/treadmill-build/treadmill`; `--help` runs. It supports Linux BLE scan,
  read-only GATT info, gated raw protocol status, profile preview, simulated
  workouts and an experimental Windows W1 workout path. Linux native BLE remains
  the default diagnostics backend; hardware playback is currently Windows only.
- A `make` invocation in `/tmp/treadmill-build` reported “Nothing to be done”
  because an executable and object files were already present there. That says
  nothing about physical control. The binary is dynamically linked to Qt 5.
- This WSL instance has no usable Linux BlueZ adapter. The CLI now invokes
  PowerShell 5.1/WinRT for Windows diagnostics and a separately gated, bounded
  raw-code motor trial. Linux Qt Bluetooth remains available as the default;
  WSL interop does not make Qt/BlueZ control the Windows adapter directly.
- The first authorized software start/target/stop trial succeeded on the W1.
  The operator confirmed physical start, console speed 1.0/incline 0, and
  physical stop with the physical controls untouched. Walking speeds through
  6.0 and incline codes 1–8 also passed with physical confirmation. Pause failed;
  physical grade, movement timing and broader faults remain unresolved.

### Runtime decision and bridge follow-up (2026-10-14)

The operator selected Windows Bluetooth through WSL for now and explicitly
requested continued native Linux availability. The CLI now supports
`--backend windows` for scan/info/nonmotor status. Linux Qt/BlueZ remains the
default (`--backend linux`); failures never cause an automatic backend switch.
The Windows path invokes the bundled PowerShell/WinRT helper in MTA mode,
validates bounded JSON reports and preserves existing logs. Both helper files
are installed under `share/treadmill/windows`.

Windows nonmotor status supports exact `--device`/`--expected-name` without
requiring invented speed/grade calibration, or a previously reviewed profile.
It requires `--confirm-diagnostics` and interactive NONMOTOR confirmation.
Q/S, EOF and signals request child cleanup; failed cleanup cannot report
success. Reports remain raw JSON with exact frames. A separate bounded motor
test and an experimental W1 workout runner now use the Windows bridge.

The updated CLI builds; eight offline bridge tests and eleven existing CLI
tests pass, as do native Windows PowerShell compilation/allowlist/approval
checks. Staged installation includes both helper sources. A real five-second
CLI Windows scan found the same FS-0F7A32 public address advertising FFF0 and
1826. Subsequent read-only info returned the correct name but service discovery
failed with Unreachable. Report:
`captures/windows-bridge-info-20261014.json`. The operator confirmed the display
was awake and the belt stopped. Broad info still returned Unreachable after that
confirmation, but a targeted eight-second FFF0 nonmotor probe succeeded with
10–60/tag 0 speed limits, 0–8 incline limits, repeated NORMAL and successful
cleanup. Capture: `captures/windows-bridge-awake-nonmotor-20261014.json`.
No motor commands were sent during this follow-up.

`treadmill control-test --backend windows --device DD:87:70:39:74:20
--expected-name FS-0F7A32 --address-type public --dry-run` prints the exact trial:
start `02530100000000000000005203`, paired target raw 10/0
`0253020a005b03`, and stop `0253035003`. Add --confirm-motor and a new --log
only after separate review and operator-at-controls confirmation. Speed units
remain unverified; raw 10 = 1 km/h is still the operator's working assumption.
This experimental raw-code test does not claim completion of the original
calibrated motor gate. No initialization replay or arbitrary targets are allowed.

The C++ trial reuses QZ's FitShow queue and encoder. Windows independently
checks current module identity (FITSHOW/FS-BT-C1/software 1.3.8), FFF mapping,
exact walking bounds and fresh NORMAL reports. The host requests stop after
five seconds of reported running; Windows attempts emergency stop on input loss,
unsafe/stale feedback or 12 seconds since start. No motor retries are allowed.
Loss of BLE or killing the Windows helper can prevent stop delivery. Full raw
logs never certify physical stopping; operator confirmation remains required.
Six fake-peer motor tests and Windows-side gate/watchdog checks pass offline.
The operator authorized and confirmed this trial. Capture:
`captures/windows-motor-trial-20261014-01.jsonl`. Module identity and walking
bounds matched; replies/countdown led to RUNNING at raw 10/0. Stop followed
5.014 seconds of reported running, with reply, STOP speeds 9/7/4/1, then NORMAL
and successful cleanup. The operator confirmed physical start/stop, console
1.0/incline 0 and untouched physical controls. No raw zero appeared; console
agreement does not independently measure belt speed or prove all units.

The operator requested a wider prepared suite, launched one at a time with
confirmation after each. Seventeen `--trial` plans are implemented and listed
by `control-test --backend windows --list-trials`: start-stop, speed
12/15/20/30/40/50/60, incline 1–8, and pause. Speed/incline steps return to
raw 10/0 before stop. Speed-12 uses three-second holds at reported 10/12/10;
other speeds hold 3/5/3 seconds. Incline trials hold 3/15/15 seconds at reported
0/requested/0, without asserting attained grade. Pause observes five seconds
after its request and explicitly stops; no resume semantics are assumed.
Windows deadlines are 12/60/80/18 seconds for baseline/speed/incline/pause,
with per-plan raw ceilings and ordered command gates. Ten offline trial tests
and Windows gate checks pass. Plans: `prepared-trials.json`. Wider evidence
worksheet: `TRIALS.md`. Baseline and speed-12/15/20/30/40 are now physically
confirmed by the operator. Individual captures and confirmations are recorded
in `validation-results.json`. Speed-12 first refused SAFETY 0x06 without any
motor writes, then refused unavailable module identity without any writes;
fresh broad info succeeded after wake and its third attempt passed.

Speed-40 reported RUNNING raw 10 after the return target, but subsequent STOP
began at raw 17. Actual belt speed versus reported setpoint remains unresolved.
Its stop write/reply/cleanup and operator-confirmed physical stop succeeded,
but the five-second observer ended after two NORMAL replies. The observer now
allows ten seconds; a delayed-stop regression covers this case.

The operator explicitly changed the workflow: batch every remaining prepared
trial, then analyze once. `control-batch --batch remaining` runs speed-50,
speed-60, incline-1 through incline-8, then pause. A single BATCH confirmation
replaces individual prompts. Each trial still performs fresh identity/limits/
status preflight and waits for explicit stop plus three NORMAL reports before
the next. Any fault/input interruption/log error ends the batch. The batch is
authorized. Its first launch in
`captures/windows-batch-remaining-20261014-01/` halted at speed-50 preflight
with SAFETY 0x06; only nonmotor queries were sent, cleanup succeeded, and no
later trial started. After wake, remaining-02 completed ten speed/incline
trials and the operator confirmed all physical changes. Pause failed, including
a focused retry after the polling fix; see current permissions below. Plan: `remaining-batch-plan.json`. Do not request a new approval
for this already authorized batch unless its scope changes.

## Hardware and identity evidence

Operator-confirmed candidate:

| Field | Current evidence |
|---|---|
| Treadmill | RunPad W1, confirmed by operator |
| Advertised name | `FS-0F7A32` |
| BLE address | `DD:87:70:39:74:20`, public address |
| FitShow module | Manufacturer `FITSHOW`, model `FS-BT-C1` |
| Module revisions | Hardware `1.0`, software `1.3.8` |
| Firmware revision | Unknown; standard firmware characteristic `2A26` was absent |
| FitShow service | `FFF0` |
| Notify characteristic | `FFF1` with notification property |
| Write characteristic | `FFF2`, Write Without Response |
| Other service | FTMS `1826` advertised/discovered; not validated for control |

Evidence files are in the ignored local directory `docs/treadmill/captures/`,
including `windows-info-verified.json` and the successive `windows-nonmotor-*.json`
reports. Do not treat module software revision as firmware or an address/name
as a permanent identity proof without rechecking at connection time.

## Live observations

The Windows helper was used for read-only discovery and later explicitly
approved nonmotor observation. It sent the FitShow capability/info requests
`5002`, `5003` and status poll `51`; it enabled FFF1 notifications and cleaned
them up after each probe. Those probes did not send motor commands.

- Speed-limit replies varied between raw minimum/maximum `10/60`, unit tag `0`,
  and `10/120`, unit tag `0x20`. Latest 120-second capture again reported
  `10–60`, tag `0`. Why the reply changes is unknown.
- Incline-limit reply consistently reported raw `0–8`.
- In observed manual runs, status `02` counted down, `03` indicated running,
  `04` indicated stop/deceleration, and short `00` indicated normal. A `06`
  safety status was also observed in earlier sessions. Short normal replies do
  not include speed or incline and do not prove a physical zero-speed state.
- During operator-controlled runs, raw speed telemetry changed with console
  changes. The latest 120-second session showed raw speed increasing from 10
  through 60, then returning to 10. The operator believes this maximum may be
  the walking-mode cap of 6 km/h.
- The operator explicitly set the working assumption for walking mode:
  displayed `1.0` means 1 km/h and `6.0` means 6 km/h. This makes raw tenths
  (`10 = 1.0`, `60 = 6.0`) plausible, but it is not an established protocol
  mapping. The alternate raw-120/tag-0x20 report remains unexplained.
- Raw incline telemetry followed console levels from 0 through 8 and back to 0.
  This confirms changing status data, not physical grade in percent or the
  amount of time for the deck to reach a commanded grade. No calibrated grade
  mapping or response-lag measurement exists.
- Manual Play/Pause produced decreasing STOP speed codes, often ending at raw
  1 or 2, followed by short NORMAL replies. No observer capture showed a raw
  zero-speed frame. The operator confirmed the belt physically stopped after
  one focused incline experiment, but did not reconfirm the physical state
  after the latest 120-second capture.
- The display previously appeared asleep with the safety magnet disconnected.
  The operator reattached the safety magnet and woke the display with
  Play/Pause. A connection between display sleep, magnet state, and BLE safety
  replies has not been established. Never defeat or disconnect the safety
  magnet to gather data.

The latest report is
[`windows-nonmotor-full-controls-120-20261013.json`](captures/windows-nonmotor-full-controls-120-20261013.json).
It ended after observing STOP codes `8`, `5`, `2`, without a zero-speed frame.
Notification cleanup succeeded. Its `motor_execution_enabled` field is false.

## Existing QZ FitShow implementation

- `src/devices/bluetooth.cpp` routes names beginning `FS-` to
  `fitshowtreadmill` under the device-selection conditions. The observed name
  matches that prefix.
- `src/devices/fitshowtreadmill/fitshowtreadmill.cpp` builds combined speed and
  incline targets and clamps speed using live capability data. With the
  default metric setting, it multiplies speed by 10 before transmission. Its
  miles option adjusts the conversion. This is a plausible explanation for
  raw 10 meaning displayed 1.0 in metric mode, but does not verify W1 units.
- The source contains start, target, stop, and pause paths and initialization
  writes. Initialization must not be replayed blindly: QZ has configuration
  paths where it can start the belt, and the protocol sequence has not been
  validated against this exact W1.
- Static code similarity and successful BLE query responses do not prove that
  QZ can safely control this treadmill. A live software-issued low-speed
  start/stop test has not happened.

## Current permissions and hardware boundary

The operator authorized bounded motor trials and then sequential execution of
all remaining prepared trials with analysis afterward. Do not re-request that
authorization. Keep physical stop accessible, the belt clear, other controllers
disconnected and fresh identity/limits/stopped preflight. A full workout has not
been launched; do not treat reported setpoints as physical arrival.

Sixteen start/stop, speed and incline plans have operator physical confirmation.
After the first batch refused SAFETY without motor writes, the operator woke the
W1 and batch remaining-02 completed speed-50/60 and incline-1 through incline-8.
Pause had no ACK and did not pause in a subsequent focused trial with continued
status polling. Keep pause/resume unavailable in the verified control path.
The shared restricted queue now polls status while awaiting an ACK; it retains
the original ACK deadline and does not retry motor commands.

## Main blockers to real workouts

1. The Windows bridge currently runs bounded trials rather than full workouts;
   the reusable workout transport and W1 evidence policy still need completion.
2. Raw walking bounds 10–60/tag 0 are validated for these trials. Alternate
   10–120/tag 0x20 capabilities remain unexplained and are refused for motors.
3. The technical specifications document gives maximum 11% and eight levels,
   but no intermediate percentage mapping or confirmed 0% minimum. Do not
   invent linear spacing. The operator wants percent units from those specs.
4. Incline status can report the target quickly, before known physical arrival.
   Deck rise/return time and an automatic moving/attained signal are unresolved.
5. NORMAL replies omit motor values, so no raw zero-speed sample is available.
   Physical stop is operator-confirmed; module software 1.3.8 is not firmware.
   Do not fabricate firmware or feedback to satisfy the generic profile gate.
6. Link loss/parent death and broader fault recovery need deliberate validation.

## Recommended next work

The latest priority is physical incline travel timing and distinguishing movement
from target attainment. `incline-timing-8` is prepared: speed 10, incline 0→8→0,
local B at each motion start, U at upper arrival, D at lower arrival, then stop.
It waits for marks (60 seconds per direction), records command/report/motion/
arrival timestamps and excludes itself from automatic batches. Operator marks
have reaction-time uncertainty; chat timestamps are not physical timing marks.
Begin with deck physically settled at level 0. The first marked timing trial
completed: rise motion 13.832 s, return motion 12.344 s; command-to-arrival
14.218/13.469 s. Level reports arrived 13.702/13.019 s before physical arrival.
No distinct FitShow/FTMS arrival flag was found. All cleanups passed. Evidence:
`incline-timing-results-20261014.json`. These are one-run operator observations,
not a calibrated automatic arrival detector. FTMS standard decoding reports
8.0% at raw level 8, inconsistent with advertised 11% max; mapping stays open.

`--observe-ftms` records 2ACD and 2ADA notifications alongside FitShow without
FTMS control-point writes. An eight-second real nonmotor probe succeeded with
both subscriptions and cleanup Success:
`captures/windows-ftms-observer-20261014-01.jsonl`. It reported NORMAL and
received repeated 2ACD frames, with no 2ADA events while stopped. Observe both
during physical movement before claiming any automatic arrival detection.

The timing/FTMS fake-peer regression, 18-plan dry-run checks, eight Windows bridge
tests and Windows C# compilation/allowlist checks pass. Windows and Linux backend
choices remain explicit; no automatic switch or motor reconnect is allowed.

## Timing repeat follow-up

Two full timing captures (01 and 02) are complete; requested 03 is absent.
Rise motion: 13.832/13.657 s; return motion: 12.344/13.281 s. Both runs have
all key marks, explicit stop, NORMAL feedback and successful cleanup. No
FitShow/FTMS arrival event identified. No universal timing threshold inferred.
The operator requested proceeding to the next trial despite missing capture 03.
`incline-timing-4` is prepared for partial travel: 0→4→0 at raw speed 10,
with the same local B/U/B/D marks and deadlines, independent Windows incline
ceiling 4, and exclusion from automatic batches. Dry plan:
`incline-timing-4-plan.json`. Percentage mapping remains unresolved.
Partial timing attempt 01 refused before motors: 50/03 incline limits had no
reply within the two-second deadline despite successful native write, while
speed limits and NORMAL replies arrived. All notification cleanup succeeded.
Use a new log path for retry; existing evidence is preserved.
Partial timing attempt 02 completed: motion rise 7.958 s and return 7.269 s,
command-to-arrival 8.458/8.181 s, all marks and cleanups successful. No new
arrival signal found. Evidence: `incline-timing-4-results-20261014.json`.
Partial attempt 03 also passed. Rise motion 8.212 s, return 7.415 s;
comparison with 02 differs by 0.254/0.146 s. No new automatic arrival signal
identified, all marks/stop/cleanup successful. Do not infer guaranteed arrival
from two operator-marked samples.
Next: confirm available angle measuring equipment and obtain settled angles
0–8 or manufacturer table before implementing requested percent targets. See `incline-timing-results-20261014.json`.

## Angle measurement follow-up

Operator has an inclinometer phone app, reading roughly 2 degrees at fully
lowered deck and 0–1 degree on the floor. Readings are provisional with unknown
bias; do not infer calibrated 3.5% baseline from one noisy app sample. Phone
placement confirmed: rigid side rail that rises with deck, around the middle.
Keep it aligned front to back in the same place; do not zero it at level 0.

`incline-measure-4` is prepared: 0→4→0, console speed 1.0, local B/U/B/D marks,
30 seconds after each arrival to take three settled angle readings. 60-second
arrival-mark timeout per direction, 210-second independent motor deadline,
240-second Windows session bound, ceiling raw speed 10/incline 4, excluded
from batches. Plan: `incline-measure-4-plan.json`. Collect initial level-0,
level-4 and returned level-0 readings; angles are operator observations reported
separately, not yet stored as a calibrated grade table. This starts the belt.
Measurement attempt 01 completed: 30-second holds verified, marks/stop/cleanup
passed. Operator reports initial and returned level 0 = 2 degrees; level-4
number was clarified as 4 degrees. Nominal gradients are 3.49%/6.99%,
unverified due to phone measurement uncertainty; no calibrated table created. Evidence: `incline-angle-results-20261014.json`.
No percent mapping inferred. Level-8 measurement is now prepared as
`incline-measure-8`: same B/U/B/D and 30-second arrival holds, speed ceiling
10/incline ceiling 8, motor deadline 210 seconds, excluded from batches.
Dry catalog and Windows independent gate checks passed. Physical measurement
attempt 01 reports angles 2/6/2 degrees at initial 0/raised 8/returned 0.
Rise mark duration 14.479 s; return marks missing, so trial stopped after
60 seconds waiting, with stop/NORMAL/cleanup all successful. Do not classify
the timing sequence as complete or infer return failure from missing marks.
Current provisional observed nodes: 0=2°, 4=4°, 8=6°, nominal gradients
3.49/6.99/10.51%, uncalibrated; remaining levels unmeasured. Marked rise 9.358 s, return 7.648 s; earlier
level-4 timing samples therefore do not establish a guaranteed upper bound.

## Automatic follow-up batch

The operator explicitly authorized automatic trial execution without individual
input, interrupting if unexpected. The prepared incline batch 1–8 with FTMS
observation was launched in `captures/windows-auto-inclines-ftms-20261014-01/`.
It halted at incline-1 preflight on SAFETY 0x06, sending only 50/02 and 51
(nonmotor) before cleanup; no motor commands or later trials. All cleanup passed.
Cause of SAFETY here is unconfirmed; previous sleeping-display episodes did this.
Ask for ordinary physical wake/safety-state resolution, belt left stopped; do not
bypass the key or fake state. No new authorization required for the same batch.
Phone readings remain inaccessible automatically; this batch collects protocol
evidence, not physical percentage calibration.

## Sleep investigation

Operator asked to retry the authorized automatic incline batch and investigate
keeping the W1 awake between experiments. Retry directory:
`captures/windows-auto-inclines-ftms-20261014-02/`: levels 1–6 completed
protocol/stop/cleanup checks; level 7 lost FitShow notifications after return
target 0. Stale watchdog attempted stop; Windows write Success but no reply or
NORMAL after stop. All CCCD cleanup timed out; batch halted, level 8 not started.
Operator explicitly confirmed physical belt stopped after fault. Short nonmotor
connection recheck is in progress; no blind motor retry. Evidence:
`auto-inclines-ftms-results-20261014.json`.

Windows `status --timeout` now accepts 1–600 seconds (default 30); Linux status
and scan/info retain 1–60. PowerShell probe and C# nonmotor session enforce the
same 600-second cap. Existing motor duration gates are unchanged. It uses only
50/02, 50/03 and 51 polling every 500 ms, with NONMOTOR confirmation,
Q/S/EOF/signal cleanup and bounded reports. Nine offline bridge tests and Windows
compilation/allowlist/long-duration scope tests pass. No keepawake benefit yet
claimed: the ten-minute observation was cancelled at the operator report of display
off. `captures/windows-idle-polling-20261014-01.json` has 625 NORMAL then
101 disabled 06/02 replies; first sleep 343.3 s after first NORMAL. All
726 status writes succeeded, max write gap 562 ms; BLE stayed responsive.
No motor writes; CCCD cleanup Success. Thus polling does not prevent sleep.
See `idle-polling-results-20261014.json`; do not call this interval the exact
firmware inactivity timeout from stop/button input. The preceding
eight-second nonmotor recheck succeeded with both limits and NORMAL, cleanup
Success. Eleven existing CLI regressions also pass.

A newer primary protocol provider document describes 51/06 reason 01 as
missing safety key and reason 02 as sleep:
https://www.fitmonster.club/SDK/FM-Protocol-Treadmill.html
It matches previous W1 disabled payload 06/02, but belongs to a different/newer
protocol source (even pause opcode differs), so treat sleep interpretation as
supporting evidence rather than verified W1 firmware semantics. No documented
nonmotor wake/keepalive command found in reviewed sources. The older FitShow
manufacturer manual is for a different 41/42 protocol family, not a basis for
sending wake or configuration commands to this W1. The official FitShow PDF download succeeded using fitshow.com and is archived
in `references/fitshow-treadmill-protocol-v1.1-2017.pdf`. It confirms 06/02 sleep
and describes 53/01 mode bit 0x80 as READY (state 9) without starting, whereas
the high bit clear starts for compatibility. This is a potential keepawake
experiment, not verified on W1, and remains a motor-capable command outside
nonmotor diagnostics. A bounded `ready-idle` trial is now prepared: exact READY mode frame
`0253010000000080000000d203`, expected status 9, 15-second hold then
explicit stop with three NORMAL replies. No target or explicit 53/09 START
is sent. Both host and Windows immediately request stop on countdown or
RUNNING; watchdog is 30 seconds. Standard identity, limits and three NORMAL
preflight remain required; asleep preflight is not bypassed. It is excluded
from automatic batches and nonmotor diagnostics. Core checks now total 48
and all passed. The operator was asked to wake normally and leave the belt
stopped for this already-authorized bounded experiment. The initial ready-idle attempt refused sleeping preflight without READY writes.
Later dedicated ready-wake attempts and outcomes are recorded below; no
keepawake behavior is verified. Dry plan: `ready-idle-plan.json`.
User now explicitly requires Internet source documents archived and committed
under `docs/treadmill/references` with URLs, timestamps and checksums.

## Build and source references

- Standalone CLI build target: `tools/treadmill/treadmill.pro`.
- Windows interop helper: `tools/treadmill/windows-diagnostics.ps1` and
  `tools/treadmill/windows-diagnostics.cs`.
- Existing QZ FitShow driver: `src/devices/fitshowtreadmill/`.
- Project state and prior software validation: `tools/treadmill/STATUS.md` and
  `tools/treadmill/README.md`.
- Hardware gates and calibration process: `docs/treadmill/03-hardware-validation.md`.
- Original scope and investigation brief: `docs/treadmill/TREADMILL.md`.

`docs/treadmill/` is ignored by Git for local planning and captures. This handoff
is intentionally tracked as a specific exception so the project goal and
hardware evidence survive a checkout; raw captures remain local.

Archived source documents were committed as `86bc1a42`; provenance and original
bytes are in `references/`. Captures and experiment worksheets remain local.

## Resume checkpoint: dedicated sleep READY trial

Temporary build/test directories vanished on environment restart; binaries were
rebuilt from repository at /tmp/treadmill-build and /tmp/treadmill-tests. Core
49, bridge 9, CLI 11, focused wake/refusal/READY-reversion/catalog regressions
and Windows gate checks pass. Google Test was extracted from a downloaded
libgtest-dev package under /tmp/treadmill-gtest, without system installation.

`ready-wake` is prepared separately from ready-idle: it accepts explicit sleep
06/02 before READY, exact identity/limits and three fresh permitted idle reports.
Safety-key 06/01 and missing/unknown disabled reasons remain refused. Generic
FitShow control still faults on all disabled states. The host intercepts only
validated sleep status in this one preflight; the independent Windows gate
checks the same rule. A maximum two-second post-request window permits only explicit sleep06/02
before READY is first established. Missing READY after that window, safety-key/
unknown disabled state, countdown or motion trigger stop. No READY retry is sent. No legacy start or target is allowed, hold 15s, deadline30s,
excluded from batches and nonmotor diagnostics. Plain ready-idle still requires
NORMAL; its attempt01 refused sleep without control writes.

The live ready-wake attempt01 could not open the selected W1 with its exact
confirmed name. No writes were sent. The read-only rescan rediscovered W1 at its exact public address/services.
Attempts02 and03 then sent READY mode 0x80 from verified sleep preflight.
The first stopped on continued sleep after about0.49s; the second allowed
the existing two-second response window and still got only06/02, no READY
acknowledgement or state9. Explicit stop writes and CCCD cleanup succeeded.
No countdown/running states appeared, but NORMAL/physical zero was not verified.
Physical wake/keepawake remains unverified. Next: await one normal physical
console wake and stopped confirmation, then run ready-idle. The question is
pending; no further sleep retry is planned. Evidence/plan:
`ready-wake-results-20261014.json`, `ready-wake-plan.json`. Do not declare the
sleep problem solved or bypass device identity to send commands.

## Awake READY result and status-query keepawake experiment

The user confirmed the console awake and requested continuation. Live
`windows-ready-idle-20261014-02.jsonl` passed three NORMAL preflight replies,
then the supposedly nonstarting READY mode-high-bit command produced COUNTDOWN
at about 494 ms. The guards requested STOP immediately; its native write and
notification cleanup succeeded. No RUNNING feedback appeared, but physical
zero/NORMAL after stop was not verified. Do not use READY as an idle heartbeat
on W1. The user now expects manual wake, consistent with the mobile app.

Plain 51 polling previously failed to prevent sleep: 343.3 seconds between
first NORMAL and first validated disabled 06/02, while Bluetooth remained
responsive. This is an observation interval, not the exact standby timeout.

An explicit Windows-only nonmotor status option `--status-heart-rate-zero`
sends documented SYS_STATUS payload 51/00 (frame 0251005103); 5002/5003 queries
are unchanged. The archived FitShow v1.1 PDF permits an appended heart-rate
byte; zero supplies no invented pulse. Default queries and motor allowlists
are unchanged. This is a hypothesis about standby, not a verified keepawake.
The helper reports status_heart_rate_zero and still sends no motor commands.
CLI rejects this option outside Windows protocol status; native PowerShell
rejects it outside probe. Eleven offline bridge checks and Windows compilation,
query allowlists and motor gates passed. Ten-minute live observation launched
as `windows-idle-heart-rate-zero-20261014-01.json`; result pending.


Important follow-up: zero-heart-rate attempt01 failed before any write at FFF0
service discovery. A scan rediscovered the exact W1 address/name/services.
Attempt02 sent 24 successful 51/00 queries over about 14 seconds; all 24 status
replies reported RUNNING code10/incline0. It was cancelled and notification
cleanup succeeded. Thus the preceding READY trial's successful STOP write did
not establish a stopped state. The user was asked to use the physical stop;
confirmation is pending. Do not launch motor trials or the stopped-idle
experiment until that confirmation. No keepawake conclusion is possible from
a RUNNING capture, and no physical cause has been established by telemetry.

Live READY trials are now retired in the CLI, PowerShell and native Reader,
before Bluetooth I/O. Catalog/dry plans remain for historical evidence and
pure guard tests. The explicit zero-heart-rate idle experiment now records
initial state, NORMAL count and time to sleep from first NORMAL. It exits early
on validated sleep06/02; any other non-NORMAL status aborts, preserving raw
frames and cleaning notifications. Neither path sends a motor command.


Final validation of this checkpoint: Windows native compilation, query/motor
allowlists, retired-READY rejection in both native and PS entry points, and
idle classification (bad checksum, initial sleep, NORMAL-to-sleep interval,
motion/countdown/key/unknown reason refusal) passed. Two focused host checks
verify both READY trials refuse before opening the helper and normal motor
trials still refuse sleep. Bridge11 and CLI11 pass. No live session remains.
Next required input: physical stop confirmation. Then run the explicit
zero-heart-rate idle probe from an awake NORMAL baseline; it may run up to
600 seconds but exits on observed sleep or unexpected state. Keepawake remains
unsolved, and hardware workout launching remains disabled.


The user physically stopped the belt and confirmed the display on. The live
51/00 observation03 returned 862 NORMAL replies then explicit sleep06/02 after
471.530 seconds from first NORMAL. All 863 51/00 writes and notification cleanup
succeeded; the display was later reported off, with unknown transition time. Thus the zero-heart-rate
query is accepted but does not keep the console awake. Earlier plain51
observation reached sleep after343.3seconds from first NORMAL. Different timing
from physical interaction prevents causal attribution to the query format.

A second nonmotor option `--status-data-sport` is being prepared for the next
manual wake. It alternates documented status51 and current exercise data52/00
at a combined500ms cadence, after 5002/5003. It is isolated from motor trials,
mutually exclusive with `--status-heart-rate-zero`, and shares the validated
NORMAL/sleep/motion guard and 600sec bound. The archived FitShow v1.1 PDF
supports 52/00 as reading current exercise amount; it says nothing about
keepawake. Treat this strictly as another hypothesis. No W1-specific
keepalive has been found. User manual wake is expected.


DATA_SPORT experiment result: attempt01 failed service discovery before writes;
rescan confirmed exact W1; attempt02 returned 478 DATA_SPORT replies, 478
NORMAL statuses and one sleep06/02 at 524.120seconds from first NORMAL.
479 ordinary 51 status and 478 52/00 data queries wrote successfully, CCCD
cleanup succeeded. Thus this documented read also did not keep W1 awake.

The user's requested sleep-timer measurement is implemented as
`--measure-sleep-timer`, a separate Windows nonmotor mode with ordinary51
polling, no motor commands and a 600sec bound. It requires initial validated
sleep06/02 and announces observer readiness, then records first NORMAL after
physical wake to next sleep06/02; motion/key/other state aborts. It is mutually
exclusive with query variants and rejected in motor paths. Offline bridge and
Windows compilation/gate checks passed. Attempt01 failed FFF0 discovery with
no writes; attempt02 was confirmed observing initial sleep and is now waiting
for the user's physical wake. Trace `windows-sleep-timer-20261014-02.json`.


Sleep-timer attempt02 did observe initial sleep06/02 but no wake: 728 sleep
replies over about403s; the user confirmed the display off at cancellation. Cancelled cleanly;
CCCD cleanup Success, CLI exit1 due to Q cancellation. No standby interval
was measured. Future timer mode now has a 90sec wake window from initial sleep
reply, still a 600sec total cap; start only when operator is ready to wake.


Operator timing clarification: the display was not watched continuously, so
"display off" reports do not timestamp the display-off event. The three
343.3/471.530/524.120-second numbers are first protocol NORMAL to first
protocol sleep06/02, not physical wake-to-sleep or display-on durations.
Do not infer that display shutdown coincides with protocol sleep, or that
query formats caused the different intervals. The dedicated timer mode will
only yield a protocol wake-to-sleep interval after it captures sleep→NORMAL
from a physical wake; physical display timing needs direct observation.


Synchronized timer attempt03: connected from documented sleep06/02, operator
physically woke while linked, first NORMAL at30127ms; 1032 NORMAL replies
followed until the 600sec cutoff, no second sleep06/02. This is a censored
protocol wake-to-sleep interval of at least569.467s, not a measured sleep time.
CCCD cleanup Success. Operator's stopwatch/display observation and behavior
after BLE disconnection are pending. Prior runs connected after the console
was already awake and did observe sleep; connecting before wake might matter,
but causality and display state are not yet established. Do not declare a
keepawake solution from this trace alone.


The operator explicitly confirmed continuous display observation in timer
attempt03: display off at stopwatch9:53 from physical wake. This was roughly
24s after the final protocol NORMAL sample/observer disconnect, although the
wake-to-first-NORMAL and stopwatch precision limit exact alignment. It is
suggestive, not proof, that BLE connection before wake affects screen standby.
A fresh ordinary51 probe was launched after the operator had already woken
the treadmill as a comparison, trace
`windows-post-wake-baseline-20261014-01.json`; outcome pending. A planned
15min before-wake repeat attempt04 failed FFF0 discovery with no writes, then
a scan confirmed exact W1; the operator woke before retry, so it became the
after-wake comparison instead. No motor writes in these attempts.


After-wake baseline completed: first NORMAL to first sleep06/02 was572.136s,
while BLE remained connected, with 1048 NORMAL and47 sleep replies and clean
CCCD teardown. Manual wake preceded probe by an unknown interval. This
undermines the conjecture that the display went off only because connection
closed in the prior attempt; the prior 600sec cutoff was likely just before
the W1's ~10min inactivity timer. Need a synchronized before-wake repeat
lasting900s to observe sleep beyond that cutoff. User is willing to repeat.


15min synchronized repeat attempts04/05 were inconclusive: 04 failed FFF0
discovery before any write; after exact W1 rescan, 05 observed 166 sleep06/02
replies and no NORMAL, then cleanly closed on its90s no-wake limit. The user
confirmed they did not wake the console. Do not label this as a failed
keepawake test or a timer measurement. Resume only when operator is present
and ready for the physical wake cue. Existing evidence supports a working
roughly10min inactivity timer; no documented safe keepawake command has been
verified. Ordinary status, status+zeroHR and status+DATA_SPORT all eventually
observed sleep, and READY started countdown on awake W1.


The operator explicitly authorized a linear grade assumption for a demo only:
level0=0%, level8=11%, each of eight intervals=1.375%. Added
`tools/treadmill/workouts/runpad-w1-discovery-demo.xml`, a 5:10 plan stepping
speeds1–6km/h at level0, then incline levels0–8–0 at1km/h. It is a simulation
workout because the hardware workout gate remains disabled. Catalog expectations
in CLI/package tests were updated for the additional installed XML; tests were
not run in this task.

## Resume checkpoint: synchronized sleep correlation

The operator reported the display had just been manually turned on while a
Windows nonmotor observer was linked. That trace recorded 424 sleep `06/02`
reports, then the first NORMAL at `2026-10-14T20:19:48.728Z`. The following
observer could not discover FFF0 on its first attempt (zero protocol writes);
after a read-only scan reconfirmed `FS-0F7A32` at the known public address, the
retry succeeded. It began 1:51 after the reported manual wake, observed NORMAL
for 483.3 seconds, then sleep `06/02` at `20:29:43.395Z`. This is 594.7 seconds
from the first NORMAL associated with the operator's wake. The operator's
previous continuously watched display-off time was 9:53, so the two observations
are consistent with an approximately 10-minute inactivity timer. The physical
button timestamp is not machine-synchronized, so treat the 9:55 protocol
interval as approximate. The first trace's earlier awake-to-sleep transition
occurred 5:32 after observer start, likely because time since prior activity
was unknown.

Polling did not prevent protocol sleep. Both traces completed with
`notification_cleanup_status=Success`; they contain no motor writes. Raw JSON is
in the ignored local directory `captures/` as
`windows-idle-wake-correlation-20261014-01.json` and
`windows-idle-wake-correlation-20261014-02.json`. No safe keepawake behavior has
been verified. Resume from the new captures and this approximate 10-minute
correlation; at that checkpoint hardware workout execution was still disabled.

## Resume checkpoint: W1 workout playback implemented

The user asked to implement actual training playback. `workout run <id>` now
supports experimental W1 execution through the Windows/WSL bridge:

```sh
/tmp/treadmill-build/treadmill workout run runpad-w1-discovery-demo --backend windows \
  --device DD:87:70:39:74:20 --expected-name FS-0F7A32 --dry-run
```

After reviewing the dry-run targets, execution requires `--confirm-motor`, a
new `--log` path and typing `MOTOR`. It requires manual display wake, an
operator at the physical controls, stopped/clear belt and the safety key. The
Windows helper verifies the exact FITSHOW/FS-BT-C1 module at software revision
1.3.8, exact W1 name/address, raw limits 10–60/unit 0 and incline 0–8, three
fresh NORMAL preflight statuses and each target's ordered bounded code. The
runner samples ramp rows every10 seconds, limits speed to raw10–60, maps grades
to the user's provisional linear levels (1.375% per code), starts each hold
after three matching reported targets, and explicitly stops at completion or
abort. Stop completion requires its write plus three NORMAL reports. Physical
incline arrival is not proven by those reports; pause is unavailable. The
hardware path is Windows only for now; native Linux diagnostics remain
available, but native Linux workout playback is not connected yet.

The native CLI builds successfully, and the Windows PowerShell script parses
and its WinRT C# helper compiles. An offline dry-run of the 5:10 discovery workout
shows 23 target holds, speed codes10–60, incline codes0–8, and an explicit stop.
No physical workout was launched in this implementation step. Test suites were
not run. Historical statements above that all hardware workouts were disabled
are superseded by this experimental Windows runner; do not describe it as a
production/calibrated W1 profile.

## Resume checkpoint: first physical discovery workout completed

After the operator authorized autonomous experiments, a Windows nonmotor check
returned repeated NORMAL status and successful notification cleanup. The
5:10 `runpad-w1-discovery-demo` then completed through the same bridge. It
reported all 23 scheduled targets in order: speeds 1–6 km/h at incline 0,
then incline codes 0–8–0 at speed 1 km/h. The host recorded successful target
writes/acknowledgements, no fault events, an explicit stop write, three NORMAL
reports after stop, and `sequence_observed=true`. The log's
`physical_stop_verified=false` is intentional: protocol feedback cannot confirm
the belt/deck physically stopped or arrived at any incline. The operator has
not yet reported what they observed physically. Raw run log:
`/tmp/runpad-workout-20261014-01.jsonl` (local machine only).

The operator also asked whether treadmill sounds can be disabled, preferably
over Bluetooth. The local FitShow protocol PDF, BLE manual PDF and QZ FitShow
implementation have not revealed a documented sound/mute control. A downloaded
manual for a different FitShow treadmill mentions volume up/down and a
sound-switch icon for Bluetooth speaker/audio output. It does not say this mutes
button, warning or pairing beeps, and it documents no Bluetooth mute command.
The manual is archived and explicitly marked as not W1 evidence in
`references/README.md`. Next, look for a W1-specific panel setting or app
control. Do not try speculative BLE writes.

## Resume checkpoint: incline FTMS status captures

After the discovery workout, short `incline-4` and `incline-8` trials with FTMS
notifications completed successfully, each with explicit stop, three NORMAL
reports and successful FitShow/FTMS notification cleanup. The standard FTMS
Treadmill Data (2ACD) Inclination field was decoded per the archived Bluetooth
GATT Supplement (signed 16-bit tenths of a percent): it reported 4.0% for
FitShow raw incline code 4 and 8.0% for code 8. Prior captures for codes 1–7
report the same one-percent-per-code pattern. This is firmware-reported data,
not independent physical calibration. The value changed within about 0.5s of
the FitShow target request in both directions, while earlier measured deck
travel takes several seconds; it therefore does not identify physical arrival.
2ADA supplied target-change/status events but no separate “incline moving” or
“target reached” indicator. Keep the user's provisional 0–11% linear training
mapping until physical measurement or stronger W1-specific evidence resolves
the conflict with the firmware-reported 0–8% values.

The new workout and level-4/level-8 captures are in ignored local
`captures/windows-workout-ftms-20261014-01/`. Both incline trials ended with
`sequence_observed=true`, successful stop writes and post-stop NORMAL status;
physical stop and deck arrival remain unverified. The treadmill was still
awake immediately after these trials; manual wake was not needed during this
work session.

The follow-up `speed-60` trial with FTMS observation reported FTMS speed 6.00
km/h at raw speed code 60. On explicit stop, FitShow status changed to STOP
(0x04) and both FitShow/FTMS speed telemetry stepped down to zero; three NORMAL
reports followed. This distinguishes belt deceleration during STOP from the
instantaneous target report, but says nothing about physical incline travel.

After the operator woke the console again, the two-minute `interval-demo`
workout completed. It reported all five merged stages in order: 1.0/0, 2.0/1,
1.0/0, 2.0/1, 1.0/0. Stop write succeeded, three NORMAL reports followed,
and no faults were logged. Physical stop remains operator-unverified. Log:
`captures/windows-workout-ftms-20261014-01/interval-demo.jsonl`.

A subsequent passive 600-second status probe started awake. First NORMAL was
`2026-10-14T21:02:48.916Z`; the first protocol sleep 06/02 arrived
316.232 seconds later at `21:08:05.147Z`. It recorded 578 NORMAL and 514 sleep
responses, no motor writes, and successful notification cleanup. The observer
remained connected; ordinary polling did not prevent protocol sleep. Physical
display-off time was not observed, so this is not a display timer measurement.
The latest probe ended in sleep; the next physical trial requires a manual wake.
Raw captures are in the ignored local directory
`captures/windows-workout-ftms-20261014-01/`.

## Resume checkpoint: workout playback follow-up (2026-10-14)

The 20-minute `uphill-walk-20` was launched after the operator said the W1 was
awake. A startup mismatch was fixed in `tools/treadmill/motortrial.cpp`: workouts
now start at the confirmed minimum 1.0 km/h and request the first workout target
after RUNNING. A 90-second temporary XML workout then passed all three target
steps, explicit STOP, three NORMAL replies and notification cleanup. Its raw log
is `captures/windows-workout-ftms-20261014-01/cooldown-stop-check.jsonl`.

The first 20-minute run reached 16:53 and covered both climbs and most of the
descent, but the Windows helper closed after its bounded live-output queue filled
while the PTY was handling verbose output. STOP was written, but three subsequent
NORMAL reports were not captured. This was an output collection failure, not a
protocol frame-count limit. A later run with stdout redirected avoided that
failure, but the treadmill itself reported STOPPING (status 4) at 5:28, while
running at raw speed 28/incline 1. No host STOP had been requested at that point.
The device then decelerated and returned three NORMAL reports after the host's
STOP. FitShow protocol v1.1 defines status 4 as “decelerating to stop”; this
capture does not identify whether a physical control or another device-side
condition initiated it. The run is not a completed workout and playback should
not be declared accepted. Captures:
`captures/windows-workout-ftms-20261014-01/uphill-walk-20-partial-01.jsonl`,
`captures/windows-workout-ftms-20261014-01/uphill-walk-20-partial-02.jsonl`.

Two subsequent module-identity lookups failed before any motor writes; an
intervening read-only probe succeeded and returned NORMAL. No further motor
trial was run after the unexplained STOPPING report. Physical grade, physical
arrival, physical stopping and sound-control support remain unverified.

After that report was reviewed, a 30-second read-only status probe returned
repeated FitShow sleep/safety `06/02` replies. It sent only the documented info
queries and status poll, no motor writes, and notification cleanup succeeded.
The operator later said the treadmill was on. A focused repeat then reached
RUNNING at raw 10/0 and target raw 20/0. After about 18 seconds at the 2.0
target, FitShow reported STOPPING (status 4) at raw 20/0. FTMS Treadmill Data
(2ACD) independently reported speed falling from 2.00 km/h; no FTMS stop/status
event identified the trigger. The host STOP followed the first STOPPING report,
then three NORMAL replies arrived and FitShow/FTMS notifications cleaned up
successfully. The six-minute repeat ended early and did not complete its target
sequence. Log: `/tmp/runpad-stop-context-20261015.jsonl`.

Two subsequent constant-target attempts with FTMS observation failed during
read-only preflight after a FitShow response deadline; neither sent motor
frames. The constant 1.0/0 comparison without FTMS observation did start and
reported RUNNING steadily at raw 10/0 for about 57 seconds. The operator then
cancelled the experiment. FitShow status 4 appeared before the host STOP; the
host stop write succeeded, three NORMAL replies followed and notification
cleanup succeeded. The trace cannot distinguish the operator cancellation from
another source of STOPPING. Log: `/tmp/runpad-stable-target-20261015-no-ftms.jsonl`.

The runner and Windows helper processes are no longer active. These logs do not
establish the physical belt state. Do not launch another hardware trial until
the operator resumes experimentation. `--observe-ftms` was enabled for
Windows-backed workout runs so future logs can capture 2ACD/2ADA alongside
FitShow; this change was built and the exact dry-run plan and simulator run
passed.

## Resume checkpoint: STOPPING follow-up trials (2026-10-15)

The operator resumed testing and reported the W1 on. A five-second read-only
Windows status probe found the exact FS-0F7A32 awake at FitShow NORMAL, with
speed bounds 10–60/unit tag 0, incline bounds 0–8, and successful notification
cleanup. A second read-only probe after one transient module-identity lookup
failure returned the same bounds and NORMAL status. The identity failure blocked
the raw-21 attempt before any motor writes; that same target was retried only
after the successful read-only probe.

Four short Windows workouts ran without FTMS subscriptions: raw 20/0 for 30
seconds, raw 20/0 for 60 seconds, raw 10/0 for 60 seconds, and raw 21/0 for 60
seconds. Each reported RUNNING at its requested code for the full hold. In every
case the host then requested STOP, FitShow entered status 4 (decelerating),
reported descending speed codes through 1, returned to NORMAL, and completed
notification cleanup. The host sequence completed, but physical stopping was
not independently verified. Logs are local under `/tmp`:
`runpad-constant-speed20-30s-20261015-01.jsonl`,
`runpad-constant-speed20-60s-20261015-01.jsonl`,
`runpad-constant-speed10-60s-20261015-01.jsonl`, and
`runpad-constant-speed21-60s-20261015-02.jsonl`. The first raw-21 attempt was
`...-01.jsonl` and contains no motor writes.

A final two-stage run held raw 20/0 for 10 seconds, then raw 21/0 for 30 seconds.
Both reported targets remained RUNNING for their full holds. Status 4 began only
after the host's explicit STOP, followed by deceleration to raw 1, NORMAL, and
successful cleanup. Log: `/tmp/runpad-transition-speed20-to21-20261015-01.jsonl`.

These controls do not reproduce the unexplained status-4 event in the earlier
workout: the prior trace showed raw 20/0 STOPPING several seconds after a
raw-21 target was requested, before any host STOP; the same 20→21 transition
and a static raw-21 hold now completed. The earlier stable raw-10 session also
ended with status 4 before host STOP, while the new raw-10 control completed its
full minute and stopped only after the host request. The trigger remains
unknown; operator input, an intermittent device condition, or another cause
cannot be distinguished from these logs. No physical belt-stop confirmation was
recorded for these latest runs. The display was not reported to need waking.
No further experiment should infer physical stop from NORMAL protocol replies.

The same 10-second raw-20 then 30-second raw-21 transition was repeated with
FTMS observation enabled. FitShow and FTMS both tracked the target change and
remained RUNNING through both holds. After host STOP, FTMS speed fell from 2.1
to 0.0 km/h while FitShow status 4 counted down raw speed to 1, then both
reported their stopped/normal state. FitShow and both FTMS notification
cleanups succeeded; `sequence_observed=true`. This controlled replay does not
reproduce an FTMS-subscription-related stop. Log:
`/tmp/runpad-transition-speed20-to21-ftms-20261015-01.jsonl`.

The W1's FTMS support is now directly confirmed: advertising includes service
0x1826; GATT discovery exposed Treadmill Data 2ACD (Notify), Fitness Machine
Status 2ADA (Notify), and Fitness Machine Control Point 2AD9 (Write + Indicate).
Live 2ACD notifications tracked speed and incline during Windows-backed
workouts, and 2ADA status notifications were received. A no-motion Request
Control / Release Control exchange succeeded. A subsequent low-speed test
accepted FTMS Set Target Speed 1.0 km/h, Start/Resume, Stop, and Release Control;
2ACD reported 1.0 km/h while running. This confirms FTMS speed control for that
one tested target. However, the 2ACC target-setting bitmap does not advertise
speed targeting, despite the accepted command. The working production motor
path remains FitShow FFF0/FFF2.

The six-minute repeat of `runpad-stop-context-6min` completed every ramp target
(raw speed 10–30; incline 0–2) with passive FTMS observation. The unexpected
FitShow STOPPING did not recur: STOPPING began only after the explicit host STOP
at +6:22, FTMS speed then fell to 0.0 km/h, and FitShow returned to NORMAL.
FTMS and FitShow notification cleanup succeeded. `sequence_observed=true`, but
physical stop was not independently verified. Log:
`/tmp/runpad-stop-context-6min-repeat-ftms-20261015-01.jsonl`.

All prepared speed 50/60 and incline 1–8 trials have already been completed;
pause was also tested and did not pause. No prepared W1 motor trials remain.
In the first FTMS run, Set Target Inclination 1.0% was sent after Start/Resume:
the command succeeded, but 2ACD stayed at 0.0%. Pre-start trials at 1%, 4% and
8% initially echoed their requested inclines in 2ACD, then returned to 0.0%
about three seconds after Start/Resume, before the explicit return-to-0
command. A success response and brief data echo do not establish that the deck
followed or held an FTMS target. Physical deck movement was not checked. FTMS
speed control at 1.0 km/h is confirmed; incline actuation remains unverified.
Capture:
`docs/treadmill/ftms-control-point-results-20261015.json`.

The 8% helper run could not confirm FTMS Stop because its deadline expired.
The established FitShow stop fallback returned Success, FTMS control was
released, and a subsequent FitShow probe reported repeated NORMAL with
successful notification cleanup.

A repeat sent the 4% target while running at 1.0 km/h. The target, return to
0%, FTMS Stop and Release Control all received successful responses, while
2ACD continued reporting 0.0% incline. The operator provided a 2° baseline,
but did not report the readings during the hold or after return; deck motion
therefore remains unverified. The stop timeout in the prior repeat was an
insufficient helper deadline; this run stopped normally. Results are in the
FTMS capture JSON.

A 60-second 4% hold with the inclinometer on the rigid side rail read 2°
throughout, the same as baseline and within the app's suspected error margin.
No physical change was measurable. FTMS accepted the target, but 2ACD showed
0.0% initially and 0.1% late in the hold, never 4%. A helper deadline expired
before the return-to-0 command; FTMS Stop and Release succeeded. A separate
0% target was acknowledged, and passive FTMS data afterward showed 0.0 km/h
and 0.0% incline. The long-hold capture is recorded in the FTMS JSON artifact.

No prepared motor trials remain. A FitShow level-4 positive-control sequence
was attempted later, but the phone readings and physical arrival marks were not
captured. The operator then instructed us to ignore inclinometer readings and
interpolate grade from the available specification: level 0 = 0%, level 8 =
11%, equal steps of 1.375 percentage points. The resulting level table is
recorded below and in the treadmill README. This is a provisional assumed
mapping rather than measured calibration. FitShow level codes and FTMS
percentages remain separate command scales; FTMS incline actuation remains
unverified. Never infer physical deck arrival from an acknowledgment or
temporary 2ACD target echo alone.

## FTMS feature bitmap analysis (2026-10-16)

The captured Fitness Machine Feature value is `9c5600000e000000`. Its second
32-bit field, Target Setting Features, is little-endian `0x0000000e`: bits 1,
2 and 3 are set, while bit 0 (Speed Target Setting Supported) is clear. The
same session's FTMS Control Point accepted Set Target Speed 1.00 km/h with
response `800201`, Start/Resume succeeded, and Treadmill Data reported 1.00
km/h while running. The W1 therefore exposes a capability bitmap inconsistent
with its observed speed-control behavior. The Bluetooth SIG FTMS test suite
requires the Speed Target Setting bit for the Set Target Speed procedure
([FTMS Test Suite, Fitness Machine Service 1.0](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2024/10/FTMS.TS_.p6.pdf)).
The evidence establishes inconsistent reporting; it does not establish whether
the firmware or module implementation is at fault. Keep the mismatch visible
in diagnostics and do not make FTMS speed control the production path based
only on one accepted low-speed command.

No application code changed for this analysis. The operator-selected
interpolated grade estimate is used for planning/playback; physical FTMS incline
actuation and deck position remain unverified independently of that estimate.

### Incline positive-control attempt (2026-10-16)

Windows interop was restored after the first launch failed with `Exec format
error` and sent no motor frames. A second launch failed device-name discovery
before motor frames; a read-only scan and GATT info then found the exact W1
(`FS-0F7A32`, public address `DD:87:70:39:74:20`, FITSHOW/FS-BT-C1, software
1.3.8), after which retry was allowed. Attempt 03 passed identity, limits and
NORMAL preflight, started at raw speed 10/incline 0, and requested incline 4.
FitShow reported RUNNING 10/4. No operator B/U marks were received within the
60-second arrival deadline, so the runner requested STOP; FitShow reported
deceleration to incline 0, then three NORMAL replies. Windows reported
notification cleanup Success. `sequence_observed=false`; physical deck arrival,
inclinometer readings and physical belt stop were not verified. Capture:
`/tmp/runpad-incline-measure-4-20261016-03.jsonl`. Do not interpret this as a
completed positive control. Before retry, confirm the stopped belt and ensure
the operator can relay movement-start/arrival marks while the CLI waits. No
further motor attempt until physical stop is confirmed.

After the operator confirmed physical stopping and authorized a retry, attempt
04 again refused before motor commands: its first FitShow status reply was
sleep/safety `06/02`. The subsequent five-second, explicitly approved
nonmotor status probe also received only `06/02` (5 replies), with successful
notification cleanup. Capture: `/tmp/runpad-status-after-sleep-20261016-01.json`.
No motor commands were sent on attempt 04. Wake the display manually, confirm
the belt remains stopped, then require a successful read-only NORMAL probe
before any further motor attempt.

After the operator reported awake, a five-second nonmotor probe received five
NORMAL (`00`) replies and cleanup Success. Attempt 05 then passed identity and
limits, started at raw 10/0, and received a successful FitShow incline-4 target
response with reported RUNNING 10/4. No physical B/U marks were received. The
host operator-stop key was issued before the mark deadline; FitShow STOP was
successful, status decelerated through incline 3/0 and speed 9/6/4/1, then
returned three NORMAL replies; notification cleanup succeeded. The attempt is
incomplete (`sequence_observed=false`), and physical angle readings and physical
stop are unverified. Capture: `/tmp/runpad-incline-measure-4-20261016-05.jsonl`.
Do not treat target telemetry as physical incline arrival. Before further motor
activity, obtain physical-stop confirmation and clarify how the operator will
send the live movement marks and endpoint readings during a trial.

The operator later confirmed the stop and requested the next experiment. An
awake/NORMAL read-only probe passed, then the fixed-hold `incline-4` trial
completed start, raw 10/0, raw 10/4, return raw 10/0, and STOP with three
NORMAL reports; notification cleanup succeeded. The CLI returned
`sequence_observed=true`. Capture: `/tmp/runpad-incline-4-fixed-20261016-01.jsonl`.
This avoids the missing live marks but only holds each reported incline target
for 15 seconds; do not equate target reports with physical deck arrival. The
operator's level-4 and returned level-0 angle readings and physical-stop
confirmation for this run are still pending. The previously recorded level-0
baseline is 2° at the same nominal phone placement. No calibrated grade table
is established by this run.

#### Incline grade estimate selected by operator

The operator instructed us to ignore the phone inclinometer readings and
interpolate from the available W1 information. Use the existing explicit
working assumption of 0% at FitShow code 0 and the product's advertised 11%
maximum at code 8, with equal spacing: 1.375 percentage points per code. The
estimated grades for codes 0 through 8 are 0.000, 1.375, 2.750, 4.125, 5.500,
6.875, 8.250, 9.625 and 11.000%. Historical phone-angle results remain in the
experiment record but are excluded from this estimate. This interpolation is
an operator-selected planning value, not manufacturer calibration or measured
physical grade.

### Short workout playback experiment (2026-10-16)

After a fresh read-only probe reported the exact W1 awake at NORMAL, with raw
speed limits 10–60/unit tag 0 and incline limits 0–8, the prepared
`interval-demo` ran end to end with passive FTMS observation. All five stages
were requested and reported in order: raw 10/0, 20/1, 10/0, 20/1, 10/0.
FitShow status remained RUNNING through the stages. FTMS 2ACD tracked the
reported speed/incline values; 395 FTMS notifications and 392 FitShow status
notifications were captured. The host issued STOP after the final 40-second
recovery, FitShow returned to NORMAL, FTMS reported zero speed, and both FTMS
subscriptions plus FitShow notifications cleaned up successfully. The CLI
reported `sequence_observed=true` and `physical_stop_verified=false`.

Log: `/tmp/runpad-interval-demo-20261016-01.jsonl`. This verifies end-to-end
command sequencing and telemetry for this 120-second, raw-speed-10/20 and
incline-code-0/1 schedule. It does not verify physical belt stop, physical
incline arrival, speed units, or the interpolated grade estimate. Further
workouts can use the selected estimate while labeling it provisional. The
20-minute workout still needs its own bounded review and explicit approval
before hardware execution.

### Discovery sampler preflight (2026-10-16)

The prepared 5:10 `runpad-w1-discovery-demo` dry run was reviewed with the
operator-selected linear grade estimate. It spans raw speeds 10–60 at incline
code 0, then codes 1–8 and back to 0 at raw speed 10; its Windows deadline is
430 seconds and it ends with explicit STOP. A fresh 5-second status probe and
then a 30-second read-only observation both returned FitShow sleep/safety
`06/02` throughout. Notification cleanup succeeded. The workout was not
launched and no motor frames were sent. The earlier READY wake command is known
to start countdown on an awake W1 and is retired, so do not use it to bypass the
sleep guard. Resume this sampler only after a physical console wake and fresh
NORMAL preflight; grade estimates remain the equal 1.375-point interpolation
per level documented above.

On the next resume, the operator reported the display awake. The attempted
read-only status probe failed before Bluetooth access because the WSL Windows
executable handler was missing: both the CLI bridge and direct `powershell.exe`
launch returned `Exec format error`, and `/proc/sys/fs/binfmt_misc/WSLInterop`
was absent. No probe or motor commands reached the treadmill. The 5:10
discovery sampler remains prepared but unrun. Restore WSL Windows interop,
repeat the nonmotor NORMAL preflight, and then launch the sampler through its
existing motor approval, bounds, deadline, and STOP gates. Do not treat the
operator's awake report as a host-verified preflight result.

## Native Linux workout transport (2026-10-16)

`--backend linux` now runs W1 control trials and workouts. The transport is a separate
BlueZ/Qt helper, `tools/treadmill/linux/treadmill-linux-helper`
(`linux-helper.pro`), that speaks the same line protocol as the Windows helper
(WRITE/QUIT in; IDENTITY/READY/RX/TX plus a final JSON report out; its backend string
is `linux-bluez`). The host finds it via `TREADMILL_LINUX_HELPER`, then
`../libexec/treadmill/`, then next to the `treadmill` binary. `workout run <plan>` now
defaults to Linux unless `WSL_DISTRO_NAME` is set.

The helper enforces its own gate, `linux/motorgate.h`, a port of the Windows
`MotorTestGate`: frame allowlist, reviewed limits 10-60/unit 0 and 0-8, three fresh
NORMAL reports before start, 1.5 s stale-status watchdog, trial deadline, one STOP only.
It also checks exact name/address, FITSHOW/FS-BT-C1/1.3.8 identity, one FFF0 service and
FFF1/FFF2 properties, and sends one emergency STOP on EOF, fault, stale status or
deadline. Not ported: pause, READY and operator-timed trials (the host refuses them).
FTMS observation is Windows only.

Qt's BlueZ backend never reports completion of write-without-response, so a helper `TX ...
Success` means BlueZ raised no error within 250 ms. Only the three post-STOP NORMAL
reports demonstrate that a stop took effect.

Hardware check on this Raspberry Pi (BlueZ, `hci0`), nonmotor only: identity verified,
limits 10-60/0 and incline 0-8 received, repeated NORMAL status, CCCD cleanup Success, exit
0, link closed. No motor frame has been sent from Linux yet. The first Linux motor run
(`uphill-walk-20`) still needs the operator's typed MOTOR confirmation.

Standby: the W1 sleeps after about 10 minutes of inactivity and nothing sent over BLE has
kept it awake (see the sleep investigation above). Whether an active run holds it awake is
unverified beyond the 8.5-minute run. Wake it at the console, then start the run promptly.

### Acceptance test passed on the Linux backend (2026-10-16)

The full 20-minute `uphill-walk-20` completed for the first time, through the native Linux
helper (`~/.local/bin/treadmill workout run uphill-walk-20`). All 47 targets were reported in
order, with no faults and no STOPPING status before the host's STOP. STOP was sent at the
planned end (1273 s), followed by three NORMAL reports and notification cleanup Success.
`physical_stop_verified` stays false by design. During the run the operator confirmed
physical incline changes (0 during warm-up, 1 at the start of the ascent) and the W1 stayed awake
past the 10-minute idle mark while running. Log: `~/treadmill-logs/uphill-walk-20-20261016-125634.jsonl`
(local only). Earlier the same day on Linux: `start-stop` and the 2-minute `interval-demo` passed.

Helper fixes found by these runs: the run deadline now counts from READY with a 45 s backstop
after the trial deadline, and a refused first BlueZ connect is retried up to three times before
any command is sent. A 60-minute plan, `workouts/uphill-walk-60.xml` (99 targets, same speed and
incline ceilings), was added; its first run is next.

### First 60-minute attempt: stopped by the operator (2026-10-16)

`uphill-walk-60` ran from 13:19:28 and ended at 824 s (13:33:12 wall clock), stage 14 of 99. The W1
reported status 4 (decelerating) at raw speed 29/incline 3 with no host command beforehand (only
51 polls), then incline fell 4 -> 3 -> 0 and speed 29 -> 1 over about 5 s, followed by three
NORMAL replies. The operator confirmed afterwards that they stopped the session by accident on
the console, so this is NOT an unexplained device-side STOPPING and is not evidence either way
about the earlier Windows-bridge events (5:28, 8:37, about 18 s, about 57 s). The host reacted
correctly: it stopped on the unexpected status 4, STOP and cleanup succeeded. A console stop is
indistinguishable in the telemetry from a device-initiated one. Log:
`~/treadmill-logs/uphill-walk-60-20261016-131928.jsonl`. The run is being restarted.

### 60-minute run on Linux: 97 of 99 targets, then the host deadline (2026-10-16)

`uphill-walk-60` (restarted 13:34:33) ran 62:10 with no faults, no status 4 and no operator input.
It reported 97 of 99 targets in order, then the host stopped it at 3724 s with `Missing status or
start deadline` because the host's per-run deadline (`total + 120 s` = 3720 s) was exhausted:
each target costs about 1.45 s waiting for three matching status reports (mean over 96 steps, 139 s
total plus 6 s startup), so a 99-step plan runs about 145 s longer than its nominal 3600 s. The
last two 10 s cooldown steps never ran. STOP, deceleration to 1, three NORMAL replies and
cleanup Success followed normally; `sequence_observed` is false only because of the missing
targets. The log is 4.4 MB (cap 64 MB). Log: `~/treadmill-logs/uphill-walk-60-20261016-133433.jsonl`.

Fix: the workout deadline is now `total + 120 s + 3 s per target` (uphill-walk-20: 1461 s,
uphill-walk-60: 4017 s, both under the 4500 s cap). This means the 60-minute acceptance run still needs
a second, complete attempt. Thermal state could not be observed: the protocol has no temperature
field and status 5 (fault) was never reported in any of our 93 logs.

## Keep-awake research (2026-10-16)

Goal: keep the W1 console awake during long sessions. All BLE-status variants had failed (plain 51,
51/00, 52/00 polling slept at 343-572 s; READY starts a countdown and is retired).

**Not a lead: `FS-0F7A32-A`.** The second advertisement at `DD:87:70:39:74:1F` is the console's classic
Bluetooth *audio* speaker (class `0x00240414` = Audio/Video, BlueZ icon `audio-card`), not a BLE control
channel; Qt's LE-only scan never sees it. It pairs without a PIN and exposes A2DP sink (0x110b),
AVRCP (0x110c/0x110e) and a PnP record (0x1200); BlueZ showed an AVRCP transport volume of 105. Both radios
remain powered while the console reports sleep. While the console was asleep, pairing and connecting
to it for about 25 s did **not** change the BLE status (06/02 throughout 85 s). The pairing was removed
afterwards (`bluetoothctl remove`). Untested: whether an *active* audio stream or the AVRCP volume
affects the standby timer (needs an awake console and a 15 minute observation), and whether AVRCP
volume 0 silences the console's beeps (relevant to the earlier sound question).

**Other findings.** The protocol documents define no temperature, keep-alive or wake field (see the
FitMonster table: status 6 reasons are only 1 = key, 2 = asleep). No manual or standby specification for the
RunPad W1 is published; the box contains a remote ("pilot") that has not been identified. The official
FitShow module manual says the app polls status 3 times per second and does not claim that this
holds the console awake. Open leads, cheapest first: (1) identify the bundled remote (IR, 433 MHz or BLE;
`btmon` while pressing it) and whether it wakes the console without starting the belt, (2) the
audio-stream test above, (3) a held FTMS session with Request Control only (never Start), which
needs a nonmotor helper mode with a one-opcode allowlist, (4) a mechanical button pusher.
Any automated button press must be tried with the operator present: Play/Pause on an awake console
may start the belt.

## FIT trainings, plans, wait-for-wake (2026-10-16)

**Wait-for-wake** (Linux backend): `--wait-wake <minutes>` polls without any motor command until the
console is awake, then runs the normal preflight; unattended it starts 5 s after the wake unless Q is
pressed, and with `--wait-wake-confirm` it starts only on ENTER (an earlier ENTER does not survive a
sleep; the test for that fails without the fix). Verified on the sleeping W1 (no motor frame written,
clean timeout); the real sleep -> wake -> start path has only been run against the fake peer and still
needs one supervised hardware check when the console can be woken.

**FIT format.** Trainings are Garmin FIT workout files written by `treadmill-fit` (official SDK in
`src/fit-sdk`) from JSON specs in `tools/treadmill/trainings/`; the CLI plays `.fit` directly and lists them
in the catalog. Speed is the standard custom speed target; incline level is a secondary GRADE target in
hundredths of a percent, because FIT defines no grade scale (Garmin devices ignore it; notes repeat it).
Verified with Garmin's own Python decoder. Not tested: loading these files into a Garmin watch.

**Library.** 18 trainings (skimo, hike, mr = mountain-run power-hike, trail) and three plans in
`tools/treadmill/plans/` (skimo 8 weeks, hiking 6, mountain running 8), all within walking mode (<= 6.0
km/h), levels 0-8 and 65 minutes; `plan.py check` enforces the guard-rails and the README states what the
W1 cannot train. No FIT training has been run on the treadmill yet: the first hardware run of a `.fit` file
is still to do. Open decisions for the operator: whether to allow running-mode speeds (the gate refuses the
10-120 report), sessions over 65-70 minutes, and keeping plans as manifests rather than FIT.

**Keep-awake:** see the research section above (the `-A` device is the classic-Bluetooth speaker).

## Garmin, running mode, long sessions, AVRCP (2026-10-16, later)

**Garmin.** Garmin's workout builder has no incline target and some watches ignore step names, so planned FIT
workouts now carry the incline where watches show it: each step's notes lead with it in plain ASCII ("Incline 8.3%
(L6), 4.0 km/h, about 330 m/h up."), and the speed target is a +/- 0.2 km/h band. The secondary grade target stays for the
CLI. What Garmin does *not* ignore is a completed **activity**: `treadmill-fit activity <run.jsonl> <out.fit>` exports a
run as a FIT activity (1 Hz records of the reported speed and incline, distance, an estimated altitude from the working grade
table, total ascent on lap and session). Checked against an independent model and Garmin's decoder; on the real 20-minute run
it gives 932 m and 23 m estimated ascent. Not tested: loading any of this into a watch or Garmin Connect.

**Running mode** (`--running-mode`, Linux only; decision: yes, but staged). Raw speed above 60 exists only with the console in
running mode (limits 10-120, unit tag 0x20; the protocol document says speeds are in 0.1 km/h units but no motor trial has
checked this here). Safeguards: separate gate profile that refuses walking limits (and vice versa); FTMS observation is
mandatory (passive: only 2ACD/2ADA notifications, no FTMS writes); above 6 km/h both the CLI and the helper stop the run if FTMS speed
disagrees with FitShow speed (> 0.6 km/h for 4 reports) or is not seen for 2 s; stage trials run-7, run-8, run-10, run-12 approach
their top speed through 6.0 km/h and record the speed in `~/.local/state/treadmill/running-validated.json` only after FTMS agreed
for 5 s at the top; workouts above the recorded speed are refused; the post-stop window scales with the speed at the stop.
Nothing above 6 km/h has been commanded on the treadmill yet.

**Correction (review, same day): FTMS is not an independent check.** Across 5,152 running samples in 22 logs FTMS speed equals
FitShow raw/10 exactly (max difference 0.00 km/h; 0.10 while decelerating), because both come from the same FITSHOW module, so it can
never disagree with the raw scale and cannot validate it. It remains a guard against gross faults only. A staged trial now unlocks its speed
only if it completes cleanly, FTMS stayed consistent, AND the operator, asked after the belt stops, reports that the console
display showed the stage speed within 0.1 km/h (recorded as `display_kmh_reported_by_operator`). Anything else unlocks nothing.
Still unasked of the operator: how running mode is selected on the console, and whether the display then reads 12 km/h as its
maximum. The whole running profile assumes the 10-120 / tag 0x20 limit report means "running mode"; that report was seen earlier
without a known cause.

**Long sessions.** Linux backend up to 120 minutes (Windows bridge stays at 70); runs of an hour or more print a notice. Nothing
past 62 minutes has run. The three 90-100 minute trainings are marked UNPROVEN LENGTH and `plan.py` keeps them out of plans.
The console may stop on its own at a fixed time (60/90/99 minutes are common); that is untested.

**FIT SDK bug found (ASan).** The SDK stores a string's length in 8 bits, so a string over 255 bytes overruns its buffer
(crashed building a long description). `treadmill-fit` now requires plain printable ASCII, a 250-byte description and bounded
names/notes; all 26 trainings, their dumps and the activity export are clean under AddressSanitizer.

**AVRCP / speaker.** `tools/treadmill/audio-probe.sh read|mute|max|restore` reads and sets the speaker's volume over D-Bus,
always removing the pairing and checking the default sink. Verified (console asleep): reads 105, sets and reads back 0,
cleans up. Side effect to know: the console REMEMBERS the volume and quantizes it; I left it muted for a few minutes during
testing, then restored it (set 105, reads back 101). Whether volume 0 silences the console's own beeps still needs the console
awake and someone listening.

### Hardware checklist, in this order (one new feature per supervised run)
1. Wait-for-wake, sleep -> wake -> start (`--wait-wake 10 --wait-wake-confirm`, then the unattended form).
2. First `.fit` run: `hike-recovery-35`.
3. The 60-minute rerun with the deadline fix (`uphill-walk-60`).
4. AVRCP: `audio-probe.sh mute` with the console awake, press buttons, listen; then `restore`.
5. Running-mode stages in order: console in running mode, you at the controls, watching the display during the 15 s top hold and answering the prompt afterwards, `control-test --backend linux ... --running-mode --trial run-7`, then 8, 10, 12.
6. A 90-minute run (`skimo-long-day-100` shortened or `hike-long-day-100`) before relying on anything over 65 minutes.
Open: the bundled remote (IR, radio or Bluetooth?) and whether it wakes the console; keep-awake is still unsolved.
