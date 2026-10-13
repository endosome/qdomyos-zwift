# RunPad W1 treadmill project handoff

Updated: 2026-10-13

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
  read-only GATT info, gated raw protocol status, profile preview, and simulated
  workouts. The command-line entry point rejects real hardware workout runs.
- A `make` invocation in `/tmp/treadmill-build` reported “Nothing to be done”
  because an executable and object files were already present there. That says
  nothing about physical control. The binary is dynamically linked to Qt 5.
- This WSL instance has no usable Linux BlueZ adapter. Windows interop can run
  PowerShell 5.1 and its native WinRT Bluetooth helper. That helper is separate
  from the Linux CLI and currently allows only three FitShow nonmotor query
  frames. WSL interop itself does not add a Windows BLE backend to the Linux
  executable.
- No software motor/start/target/pause/stop command has been sent to the W1.
  All software-side movement claims remain unverified. Prior physical movement
  was initiated and stopped by the operator using the treadmill controls.

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

The operator most recently authorized unrestricted *nonmotor query* types.
This allows investigation and sending informational/status queries after their
effects are understood. It does not mean that a query response proves motion or
calibration, and it does not remove the distinction between a query and a motor
command. The operator has also asked to verify live motor control and has stated
the primary goal above; the control executable and physical acceptance test are
still outstanding.

The original validation plan in
[`03-hardware-validation.md`](03-hardware-validation.md) requires a bounded
first motor test, validated identity/limits/units and safe initial state,
operator at the physical controls, and an accessible physical stop. Do not
launch a full workout as the first motor trial. Hardware start/target/pause/
stop operations remain blocked in the current CLI and the Windows helper.

## Main blockers to real workouts

1. There is no runnable application in this checkout that controls the W1
   through Windows Bluetooth from WSL. The current helper is PowerShell/WinRT
   diagnostics; the standalone C++ CLI uses Linux Qt Bluetooth.
2. The FitShow control protocol has not been exercised by software on this W1.
   Initialization requirements, start and stop frame effects, acknowledgments,
   retry behavior, and start/countdown handling remain unverified.
3. Speed mode/unit and the conflicting capability replies are unresolved.
   Walking-mode `1.0 = 1 km/h` is a user-directed working assumption only.
4. Incline feedback is visible, but mapping raw codes to measured percent grade,
   actuation lag, and whether status represents target or attained position are
   unknown.
5. Stop feedback does not include observed raw zero. Physical stopping was
   confirmed manually once, but software stop and stop feedback are untested.
6. The latest known post-observation physical state is uncertain. Confirm it
   directly before any motor test.

## Recommended next work

1. Choose the runtime host: a native Linux machine with working BlueZ, or a
   Windows-native Qt/QZ executable using Windows Bluetooth. If continuing with
   WSL, make the Windows interop bridge a supported local backend; do not claim
   the Linux Qt backend controls Windows Bluetooth.
2. Implement the smallest local executable path that reuses the QZ packet and
   FitShow control logic. Keep query operations and motor commands explicitly
   distinct, log exact frames and outcomes, and ensure physical stop remains
   reachable if the app disconnects.
3. Review the exact command sequence and fresh device state before a bounded
   first control test. Use the verified device name/address and reported limits;
   do not use a guessed speed unit or grade conversion. First test should use
   the lowest supported speed, flat incline, a short hold, and explicit stop.
4. Capture requested commands, replies, raw telemetry, physical belt behavior,
   and stop completion. Repeat only to resolve a named evidence gap.
5. Calibrate the complete speed range policy and all incline codes/grade
   response. Establish reliable start/stop/pause behavior and stale-feedback
   fault handling.
6. Only after those checks, enable the controller adapter for a short complete
   workout. Then validate planned session execution, interruption recovery,
   logs, and failure handling on the physical treadmill.

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
