# Implementation status

Experimental Windows/WSL playback for predefined W1 workouts is implemented,
but has not been physically run end to end. It uses the user's provisional
linear grade assumption and exact-device/raw-limit gates. Native Linux workout
playback is not connected. See the current checkpoint in
[HANDOFF.md](../../docs/treadmill/HANDOFF.md#resume-checkpoint-w1-workout-playback-implemented).

Bounded Windows/WSL motor control is physically confirmed for start/stop,
walking speeds 1.0–6.0 km/h and incline codes 1–8. Pause did not work in the
verified path. Experimental W1 workout playback is now implemented through the
Windows/WSL bridge; it has not yet been physically run end to end. Playback is
pinned to the exact W1, raw bounds and 1.0–6.0 km/h, and requires manual wake,
interactive MOTOR approval and physical stop access. The RunPad W1
specification provides only a maximum 11% grade and eight levels, not a percent
for each level; firmware revision and raw speed zero are also unknown. Native
Linux remains available. The display requires manual wake. In a synchronized
idle observation, the operator's reported wake aligned with FitShow NORMAL at
20:19:48 UTC; status changed back to protocol sleep at 20:29:43 UTC, 594.7s
later. The display had previously been observed to turn off at 9:53. Polling
did not prevent sleep. This supports a roughly 10-minute inactivity timer, but
physical button and display timestamps are approximate; no keepawake mechanism
is known. Historical observations below are superseded by this current summary
where later trials provide stronger evidence.

`incline-timing-4` and `incline-timing-8` are prepared and tested offline with local B/U/B/D marks;
the first marked full traversal measured rise motion 13.832 seconds and return
motion 12.344 seconds, with operator reaction-time uncertainty. Level reports
preceded arrival by 13.702/13.019 seconds; no arrival flag was identified. It requires a physically settled
level-0 baseline, bounds each direction to 60 seconds and is excluded from
batches. `--observe-ftms` records 2ACD/2ADA with source UUIDs without FTMS motor
writes. A real eight-second nonmotor probe passed with both subscription cleanups
Success; stopped 2ACD data arrived but no 2ADA events. Movement/arrival detection
remains unproven. The technical specs list maximum 11% and eight levels without
per-level percentages. The discovery-demo workout applies the user's explicitly
provisional equal-spacing assumption; it is not a measured W1 grade mapping.

## Dedicated READY wake trial

ready-wake adds a separately bounded motor-capable trial which accepts documented
sleep 06/02 only before READY, while safety-key and unknown disabled reasons
stay refused. Other trials and generic FitShow control retain their guards.
Core49, bridge9, CLI11, Windows gates and focused wake/refusal/reversion/catalog
checks pass after rebuilding in the resumed environment. Live attempt01 failed
at device open with no writes. Rescan rediscovered W1. Attempts02/03 sent
READY from sleep but received continued06/02, no READY acknowledgement/state9.
The latter waited the two-second response window and stopped after2.002s.
Native stop/CCCD cleanup succeeded; no countdown/running feedback appeared.
Physical zero/NORMAL was not verified. Await an ordinary physical wake for
ready-idle from NORMAL; keepawake behavior remains unverified.

## Sleep investigation

Windows nonmotor status now supports a continuous 1–600-second observation,
retaining the default 30 seconds. Linux status and scan/info stay 1–60 seconds.
Only verified information/status queries are used, with existing confirmation,
input/signal cleanup and output limits. Nine bridge regressions and Windows
compilation/allowlist/bounded-duration checks pass. This supports a ten-minute
idle polling experiment; actual polling failed to prevent sleep: 625 NORMAL replies then 101 disabled
06/02, 343.3 s after first NORMAL, with all query writes successful and BLE
still responsive. No motor commands; cleanup Success. The official FitShow
protocol identifies detail 2 as sleep and provides an untested READY-mode
candidate (53/01 with mode bit 0x80), still treated as motor-capable.

## Windows backend follow-up (2026-10-14)

The operator selected Windows Bluetooth through WSL while retaining native
Linux support. The CLI now dispatches scan/info/status with `--backend windows`;
the existing Qt/BlueZ path remains the default (`--backend linux`). The Windows
backend invokes the installed PowerShell/WinRT helper in MTA mode with argument
arrays, bounds execution/report size, preserves existing logs, and checks the
report's backend/command/nonmotor scope. Protocol status accepts the confirmed
address/name or a reviewed profile, and requires interactive NONMOTOR confirmation.
Identity-only probes do not need or supply calibration data. Cancellation requests notification cleanup
through the child's input pipe; missing cleanup cannot produce a successful exit.
Windows reports contain exact raw frames and final cleanup status, with no
calibration or physical-stop inference. A separate `control-test` command now
reuses the production QZ FitShow queue/encoder through a streaming Windows
transport for one bounded raw 10/0 start/target/stop trial. It has dry-run and
interactive MOTOR approval, exact live module identity/bounds gates, host and
Windows watchdogs, and bounded attempt/completion logs. It never claims physical
stopping and does not enable workout execution or calibrated target changes.

Eight offline child-process tests and the eleven existing CLI tests pass;
Windows PowerShell compilation/query allowlist/approval checks also pass.
A real five-second read-only scan through the CLI found FS-0F7A32 at the
previously confirmed public address, advertising FFF0 and FTMS 1826. No motor
commands were sent. Subsequent read-only info saw the same name but GATT service
discovery returned Unreachable; the report is saved locally as
`docs/treadmill/captures/windows-bridge-info-20261014.json`.
The operator subsequently confirmed the display was awake and the belt stopped.
Broad GATT info remained Unreachable, but an eight-second targeted nonmotor
FFF0 probe succeeded, reporting speed 10–60/tag 0, incline 0–8, repeated NORMAL
and successful notification cleanup. No motor commands were sent. Six fake-peer
motor trial tests and the expanded Windows-side gate/watchdog tests pass offline.
The first authorized MOTOR trial passed: exactly one start, raw 10/0 target and
stop, with weak replies, countdown, RUNNING at raw 10/0 for 5.014 seconds, STOP
speeds 9/7/4/1, then three NORMAL reports and successful cleanup. The operator
confirmed start/stop, console 1.0/incline 0 and untouched physical controls.
No raw zero was reported. Capture:
`docs/treadmill/captures/windows-motor-trial-20261014-01.jsonl`.

Seventeen named trial plans are prepared: start-stop; speed steps to raw
12/15/20/30/40/50/60 and back to 10; incline codes 1–8 and back to 0 at raw
speed 10; and pause observation followed by explicit stop. All launches are
individually bounded, with exact printed plans and independent Windows-side
sequence/ceiling/deadline gates. The operator later authorized all remaining
trials in one sequential batch with combined review afterward. Ten fake-peer trial
tests and Windows-side ordered gate tests pass. A matching incline report is
not attained position or calibrated grade. Resume semantics, stop repeatability,
interruption/link-loss behavior, conflicting capability replies, full speed
calibration and all incline grades remain outstanding.

Operator-confirmed trials now include start-stop and speed-12/15/20/30/40.
Speed-40 completed all commands, but its five-second stop observer received only
two NORMAL replies before ending; the operator confirmed physical stop. The
observer now allows ten seconds. RUNNING raw 10 followed by STOP raw 17 also
leaves setpoint versus attained belt-speed semantics unresolved. Two new offline
checks pass for delayed stopping and batch fault isolation. `control-batch`
runs a selected group with fresh preflight and explicit stop between trials,
preserves existing logs, and halts after any failure. The remaining 11-trial
batch was authorized and launched, but its first speed-50 preflight returned
SAFETY 0x06. No motor commands or later trials were sent; cleanup succeeded.
The operator has been asked to wake the display and leave the belt stopped;
the same authorized batch is ready for immediate retry. Physical review is pending.

| Roadmap task | Status | Evidence / next dependency |
|---|---|---|
| M0 assessment | Complete | Eight local planning documents under ignored docs/treadmill |
| 1.1 standalone scan | Implemented; physical scan unverified | Qt 5.15.18 build and argument validation |
| 1.2 read-only info | Implemented; physical GATT unverified | Bounded explicit-address inspection, no write/subscription calls |
| 1.3 exact W1 identity | Operator confirmed selected treadmill; BLE module identified | FITSHOW FS-BT-C1, hardware 1.0, software 1.3.8; firmware revision not exposed |
| 1.4 QZ link spike | Isolated driver compiled and linked | Seven QZ sources plus the adapter; Core/Bluetooth/Positioning; no GUI or virtual peripheral |
| 1.5 nonmotor probe | Eight Windows probes completed after explicit approval | Latest capture reported speed bounds 10–120/unit tag 0x20 (earlier 10–60/tag 0), then NORMAL→START→RUNNING with raw speed 10/incline 0 through elapsed second 12; no stop/change captured; no motor commands |
| 1.6–1.7 calibration/start-stop | Manual start/pause captured; speed unit and incline steps unverified | Operator reports display speed 20.0→1.0, but latest RUNNING frames stayed at raw code 10/incline 0; telemetry/display mismatch unresolved. Unit and raw zero unknown |
| 2.1–2.3 controller/simulator | Implemented | Core-only deterministic and failure tests |
| 2.4–2.7 restricted driver policy/queue/parser | Implemented; hardware unverified | Offline queue/parser tests and actual isolated-driver tests |
| 2.8 real adapter/profile validation | Implemented; physical acceptance pending | Offline injected-frame tests; actual Qt binding compiled/linked; live identity/bounds/evidence gates |
| M3 workout engine | Implemented offline; W1 Windows playback connected | Generic XML/ramp sampling, linear grade assumption, bounded 10-second target steps |
| M4 CLI | Simulation and experimental Windows W1 run path implemented; physical run pending | Catalog, controls, logs, interactive approval, exact identity/bounds and explicit stop |
| M5 hardening/package | Software support implemented; physical release acceptance pending | CI/install/sanitizer support; full QZ library and selected existing regressions validated |

## First hardware checkpoint: ready for read-only discovery

Use the intended Linux controller host, with the W1 powered and available for
Bluetooth discovery. Close other applications controlling it. Keep physical
controls available; connection itself can change controller ownership even though
no motor commands are sent. Do not infer unique identity or compatibility from a
similar device name.

```sh
treadmill scan --timeout 15 > w1-scan.json
treadmill info --device AA:BB:CC:DD:EE:FF --timeout 15 > w1-info.json
```

Select the exact address from the scan; replace the placeholder. Share those
reports with the exact model/firmware and Linux/BlueZ/Qt versions. Addresses can
be redacted consistently in shared copies. Advertised services may be incomplete.
Info may read values during Qt service discovery; no characteristic/descriptor
writes or subscriptions are requested. Model/firmware may remain unknown.

## Next hardware checkpoints

The restricted FitShow adapter and nonmotor query/subscription mode are
implemented and tested offline. After reviewing GATT evidence, the status probe
requires a separately reviewed write-enabled step; it is not part of the
read-only tool. Establish actual protocol framing,
initialization, speed units/limits and discrete incline mapping before motor use.
Only then propose a bounded minimum-speed start/stop test with explicit operator
confirmation and physical-stop access. A full workout is the final hardware
checkpoint after simulator safety tests and control/feedback validation.

## Current implementation boundaries

Simulator capabilities and timing limits are synthetic. It can confirm simulated
movement/stopping, not W1 movement. The future hardware controller must expose
raw valid feedback and calibrated discrete grade mapping rather than infer them
from QZ public metrics. Pausing is implemented/verified only in simulation.

The QZ driver has a tested opt-in restricted path with explicit GATT mapping,
asynchronous priority-stop queue and raw capability/status events. Legacy QZ
behavior remains the default. The workout CLI still refuses hardware execution.
The isolated QZ driver and adapter compile/link. The full QZ static library builds. Selected existing FitShow/training/metric regressions pass with the Bluetooth system bus disabled (three inapplicable exclusion cases are skipped).

The initial offline implementation ran no physical treadmill, motor commands,
Bluetooth connections or scans. Subsequent read-only Windows discovery is
recorded below. Existing device support must not be claimed from simulation or
byte fixtures.

## Library boundary and maintenance

Controller data types/interface, simulator, workout model and runner depend on
Qt Core only and are built without BLE in the test target. They are suitable for
a later internal library target; terminal/session and diagnostics remain outside
that boundary. Do not publish a stable SDK until real-controller behavior and
profile mapping are validated. The current framing helper is shared with QZ to
avoid a separate protocol implementation.

Code remains under the repository's applicable GPL terms. Install the repository
LICENSE and keep corresponding source/build instructions with distributed
artifacts. Qt dependencies are dynamically linked; this install target does not
bundle Qt/BlueZ or complete an audit of the full QZ dependency tree.

## Software checks completed without a treadmill

Qt 5.15.18/C++17 builds for the standalone CLI, isolated real driver/adapter and
full QZ static library. Core parser/queue/profile/runner/adapter tests, actual
isolated-driver tests, offline CLI/terminal tests and address/undefined-behavior
sanitizers are included. Staged installation carries external XML plans, the
unverified profile example, documentation and applicable repository license.
Tests use synthetic transport data and an unavailable system Bluetooth bus;
none represents physical W1 validation. Existing source warnings and nonfatal
Qt QML foreign-type metadata warnings are not treated as new hardware evidence.

The remaining hardware acceptance tasks are identity/protocol confirmation,
required initialization, speed units/resolution, all incline mappings and actual
feedback semantics, bounded start/stop, pause if needed, and a conservative full
session. Unlocking hardware workouts and validating device-specific deadlines
remain explicit future steps; no automatic enablement from a profile file exists.

Latest offline results: 46 Core tests, 4 isolated-driver tests and 11 CLI/terminal
tests pass. Core tests pass under address/undefined-behavior sanitizers. The
selected existing QZ regressions have 32 passes and 3 documented skips. The
adapter independently latches faults and revokes queued motor authorization when
reports become stale; fresh packets cannot silently resume a faulted session.

The complete external 20-minute workout also passes through the production
FitShow queue, raw decoder and adapter using a delayed synthetic peripheral.
Startup waits for asynchronous capabilities/identity and three stopped reports;
missing initial reports expire the bounded preflight without arming. Priority
abort overtakes a delayed target reply and never transmits another motor target.
Integration fault tests cover changed capabilities, replies after the no-retry
deadline, missing motor reports and reconnection with old replies. Fault-latched
feedback or unavailable transport produces stop-unconfirmed, even when the
synthetic peripheral has stopped; it cannot certify a physical result.

Clean Debian 13 container builds pass the same 44 Core, 4 driver and 11 CLI
tests. Four separate runtime-package checks validate installed catalogs, both
full workouts, profile preview/hardware refusal and license/corresponding source.
Runtime tests have no network, host mounts, Bluetooth bus or source checkout.
The package recipe pins the base image and records dependency versions; APT
updates mean byte-for-byte build reproducibility is not claimed.

The installed internal API contract documents borrowed/owned lifetimes, one Qt
event-loop thread, monotonic clock domains, regular advancement, generation/ID
semantics, explicit abort-before-shutdown and the future library boundary.
All four follow-up software tasks (full FitShow sessions, fault races, clean
packaging and API documentation) are complete. The next planned acceptance
step requires the postponed physical device: read-only discovery and identity.

## WSL hardware discovery follow-up

The WSL environment has no BlueZ adapter. Following the working open_oura
approach, a separate Windows PowerShell/WinRT scan/info helper uses Windows'
native adapter without USB passthrough. Windows interop was restored by the
operator. A 15-second scan succeeded and found a candidate advertising FitShow
FFF0 and FTMS 1826. The operator confirmed the candidate is their treadmill.
Initial read-only GATT discovery in PowerShell's default STA host timed out,
including after other apps were disconnected. MTA discovery succeeded, then
succeeded again through automatic MTA re-entry. The helper now uses that mode;
reports identify the last operation so failures can be located. This is evidence
for a host threading issue, not proof of the treadmill's sleep/wake behavior.

Discovered FitShow FFF0 contains FFF1 Notify and FFF2 WriteWithoutResponse,
matching the existing driver mapping. FTMS 1826 is also present. Standard
Device Information reads returned manufacturer FITSHOW, model FS-BT-C1,
hardware 1.0 and software 1.3.8. Firmware characteristic 2A26 is absent; software
revision must not silently replace firmware in the current identity gate.
Standard FTMS feature/range values were captured as raw bytes; they do not
validate physical units, controls or incline calibration. During this read-only
checkpoint, no motor commands, pairing, writes or subscriptions were sent.
The helper compiles on Windows PowerShell 5.1 and rejects malformed addresses
before opening a device. Scan captures remain local under ignored docs/treadmill.
The Linux BLE backend and W1 motor/feedback compatibility remain unverified.

A Windows nonmotor probe was run for 20 seconds after explicit operator approval. It
requires separate explicit NONMOTOR authorization, exact address/name, matching
FFF1/FFF2 properties and a bounded duration. Its only framed writes are 5002,
5003 and 51, plus CCCD notification enable/cleanup. Offline Windows checks pass
for compilation, address validation, three exact frames, forbidden commands and
both approval gates. Hardware workout execution remains disabled.

The probe received checksum-valid speed bounds (raw codes 10–60, unit tag 0),
incline bounds (raw codes 0–8, pause flag set) and repeated status frame
`025106025503`. QZ classifies status 0x06 as SAFETY; this short response provides
no motor telemetry and its extra byte 0x02 has no established physical meaning.
CCCD notification cleanup returned Success. No motor commands or pairing were
sent. The operator reported a blank display and disconnected safety magnet;
after normal magnet attachment, a second 20-second probe returned NORMAL (0x00)
as `0251005103`. Both probes cleaned up notifications successfully. A later 60-second nonmotor
observation returned SAFETY (0x06) on all 104 status replies, with no speed or
incline telemetry; cleanup succeeded. The operator then reported that one
Play/Pause button press woke the previously blank display without starting the belt. A following 20-second observer received
NORMAL (0x00) on all 31 status replies, plus the same bounds and no motion
telemetry; cleanup succeeded. This coincidence suggests display sleep may be
related to the SAFETY reply, but does not establish causation. The magnet cause
is not established. NORMAL is a short status-only reply: it contains no
speed/incline feedback and does not certify physical stopping. These raw
replies do not validate speed units, grade mapping, pause, stop or movement. Two regression tests use sanitized observed frames to check
decoding and revocation of queued control on the safety response.

In a later 60-second observer session, the operator manually started the belt
after the reported three-second countdown and pressed physical Play/Pause after
about three seconds of movement. The trace contains status START (0x02) with
countdown values 3, 2 and 1, then RUNNING (0x03) with raw speed code 10 and
incline code 0. Following the physical pause, it reported STOP (0x04) with raw
speed codes 9, 6, 4 and 1, then a short NORMAL (0x00) reply. No status-4 sample
reported raw speed zero. The operator reports the belt stopped/paused; the trace
does not provide a verified zero-speed value. Notification cleanup succeeded.
Only the nonmotor 5002, 5003 and 51 queries were sent; no software start, stop,
pause or target command was sent. This validates receipt of live FitShow status
frames during operator-controlled movement, not the Linux Qt backend or
software motor control. Raw speed units and incline calibration remain unknown.


In the latest 60-second observer capture, the operator reported that the console
showed speed 1.0 and incline 0 after physical Play, then speed decreasing from
1.0 to 0.0 in 0.1 steps after physical Pause; the speed unit was not specified.
The accompanying BLE trace completed and cleaned up successfully but contained
only 106 SAFETY (0x06) status replies after the bounds responses, with no
RUNNING, STOP or speed telemetry frames. It is unresolved whether the physical
button sequence occurred during the capture or why the console report and BLE
status differ. Do not use the reported display values as BLE calibration until
a capture correlates them.


A repeat synchronized observer run captured RUNNING and STOP frames during the
operator-controlled Play/Pause test. The BLE trace reported RUNNING (0x03), raw
speed code 10 and incline code 0. The FitShow elapsed-seconds field progressed
from 0 to 10. STOP (0x04) frames then reported raw speed codes 10, 7, 4 and 1 at
elapsed second 11, followed by short NORMAL (0x00) status replies. The observer
did not receive a raw zero-speed frame. In an earlier attempt, the operator
reported console speed 1.0 and incline 0 while running, then speed falling to
0.0 during pause; no unit was given. Those readings were not repeated for this
capture, so the numeric match to raw speed code 10 remains unconfirmed.
Notification cleanup succeeded, and only 5002, 5003 and 51 queries were sent.
No software motor, pause or timer control was used.


A further 60-second observer run requested for console speed/incline step changes
returned speed limits `025002780a002003` (max raw 120, min raw 10, unit tag
0x20), differing from the earlier reply with max raw 60 and unit tag 0. Incline
limits were unchanged. Status progressed from NORMAL through START countdown
3/2/1 to RUNNING, with raw speed code 10 and incline code 0 through elapsed
second 12. No speed/incline telemetry change or stop report appeared before the
observer ended. Notification cleanup succeeded; only the three approved
nonmotor queries were sent. The cause of the changed speed limit reply and the
physical state after the capture need operator clarification.


After that capture, the operator reported changing console speed up to 20.0 and
back down to 1.0, with no units displayed. The capture's RUNNING frames remained
at raw speed code 10 and incline code 0 throughout, so those displayed values
were not reflected in the captured BLE telemetry. It is unknown whether the
console changes occurred within the observer window or why the values differ.
Incline has not yet been adjusted.

A repeat 60-second nonmotor capture subsequently recorded RUNNING speed codes
increasing from 10 through 20 and returning to 10. The reported speed-code
sequence therefore changed during this capture; display units and the console
reading during this specific run still need operator confirmation. Incline
telemetry changed from code 0 up to 8 and back to 1 during the same run; whether
the operator adjusted incline needs confirmation. Physical pause produced STOP
(0x04) speed codes 9, 6, 4 and 1 at incline code 8, then short NORMAL (0x00)
replies. No zero-speed frame was received. Notification cleanup succeeded. Only
the approved nonmotor 5002, 5003 and 51 requests were sent; motor execution
remained disabled.

A focused 60-second incline repeat recorded all running incline codes from 0
through 8, then back down to 0 while the running speed code remained 10. The
operator pressed physical Play/Pause after returning to incline 0. STOP (0x04)
frames reported speed codes 8, 6 and 3 at incline 0, followed by short NORMAL
(0x00) replies; no zero-speed frame appeared. Notification cleanup succeeded.
Only the approved nonmotor 5002, 5003 and 51 requests were sent, with motor
execution disabled. The capture confirms the incline telemetry reached code 0
before the stop sequence; the operator subsequently confirmed the belt stopped.

The operator subsequently confirmed that the belt is physically stopped. The
incline trace's raw status field followed each reported level from 0 to 8 and
back to 0, showing that the treadmill sends changing incline feedback over
Bluetooth. The capture does not timestamp individual console target changes, so
the lag between selecting a level and reaching/reporting it is not yet measured.

A combined 60-second nonmotor capture recorded fresh speed limits of raw 10–60
with unit tag 0 and incline limits 0–8. During RUNNING, raw speed rose from 10
to 20, then fell to 15 and remained there while incline telemetry changed from
0 through 8 and back to 0. Thus this capture did not show speed returning to raw
10 before the operator's physical Play/Pause. STOP (0x04) then reported speed
codes 14, 12, 9, 6 and 3 at incline 0, followed by NORMAL (0x00); no zero-speed
frame appeared. Notification cleanup succeeded. Only nonmotor queries 5002,
5003 and 51 were sent; motor execution remained disabled.

After this capture, the operator said the console may have reached displayed
speed 1.0 only after the 60-second observer ended, but was uncertain. This is
not present in the trace: the last RUNNING speed code was 15, followed by STOP
codes down to 3 and then NORMAL replies. A longer or renewed observation would
be needed to correlate any later display change; speed units remain unknown.

A 120-second nonmotor capture then recorded bounds min/max raw speed 10/60 with
unit tag 0, and incline limits 0–8. RUNNING speed telemetry rose from code 10
to the reported maximum code 60, then returned to 10 before incline was changed.
The operator reports the console would not increase to displayed 20.0 on this
run; the peak display value was not recorded, so raw code 60 cannot yet be
mapped to a display value or unit. During the incline sweep, sampled codes
included 0, 1, 2, 4, 5, 7 and 8, then returned to 0; intermediate steps may
have occurred between the 500 ms status polls. Physical Play/Pause produced
STOP (0x04) speed codes 8, 5 and 2 at incline 0 before the observation ended.
No zero-speed frame was captured. Notification cleanup succeeded, and only
nonmotor queries 5002, 5003 and 51 were sent; motor execution remained disabled.

The operator believes raw maximum 60 may be the walking-mode limit of 6 km/h.
This is consistent with a tenths-of-km/h scale (raw 10 = 1.0, raw 60 = 6.0),
but remains an unverified interpretation because the display showed no units.
The earlier max-120/unit-tag-0x20 reply may reflect a different mode or setting;
that relationship has not been established.

For subsequent walking-mode analysis, the operator explicitly instructs us to
assume displayed speed 1.0 equals 1 km/h and 6.0 equals 6 km/h. Use that as the
working scale for walking mode; it does not resolve the separate raw-120/tag-0x20
response.


2026-10-14 keepawake checkpoint: ordinary 500 ms status polling did not prevent
sleep (first NORMAL to 06/02 about 343 s). READY mode 0x80 did not wake sleeping
W1 and started countdown on awake W1; its guard stopped the attempt. READY is
unsuitable as an idle keepalive. Manual wake is expected by the operator.
Windows-only `--status-heart-rate-zero` is prepared as a documented nonmotor
51/00 status-query experiment, without changes to default or motor queries.
Offline bridge suite 11 and Windows framing/gate checks pass; live ten-minute
observation is pending. No keepawake solution has been verified.


Follow-up: the 51/00 capture started in RUNNING (24 replies at speed code10),
so it cannot measure idle standby. Physical stop confirmation is pending.
READY live trials are retired before Bluetooth I/O; their successful STOP
write had not established a stopped state. The explicit zero-heart-rate idle
experiment now aborts on non-NORMAL status and ends on validated sleep06/02.
It records initial state, NORMAL count and elapsed time from first NORMAL to
sleep. Keepawake remains unresolved.


The live zero-heart-rate status probe reached sleep06/02 after 471.530 s from
first NORMAL (862 NORMAL replies), with successful BLE cleanup; the operator
later reported the display off, with no timestamp for when it went dark. This variant does not prevent protocol sleep. The Windows-only
`--status-data-sport` experiment alternates documented status51 and read-only
current-exercise-data52/00 at a combined 500 ms cadence, from a manually woken
NORMAL baseline. It is bounded to 600 s, stops on explicit sleep or motion,
and remains an unverified keepawake hypothesis.


The documented DATA_SPORT 52/00 query also did not keep the W1 awake: it
replied 478 times, but sleep06/02 followed 524.120 s after first NORMAL.
`--measure-sleep-timer` is a separate nonmotor Windows observer for measuring
standby: connect while asleep, wait for its confirmed-sleep cue, wake with
physical controls, and record first NORMAL to next sleep06/02 (up to 600 s).
The elapsed time is protocol-observed and has roughly polling-interval
uncertainty relative to the physical button press.


The first sleep-timer run saw 728 sleep06/02 replies, no NORMAL transition,
and was cancelled after the operator confirmed the display stayed off. No timer
value was measured. Future runs close after 90 s without a physical wake after
the first sleep reply, while retaining the 600 s total limit.


The operator did not watch the display continuously. All reported 343.3,
471.530 and 524.120-second intervals end at protocol sleep06/02, not at a
measured display-off event. They start at the first protocol NORMAL reply,
not the physical wake action. The differing values do not establish that
any query format changed the standby timer.


A synchronized timer run connected while the W1 was asleep and the operator
physically woke it during observation. The first NORMAL reply was at30.127s;
there were 1032 NORMAL replies over the next569.467s with no next protocol
sleep before the 600s cutoff. This is a lower bound, not an exact standby
time. Display-off and post-disconnect observations are pending. Connection
before wake is an unverified possible factor.


The after-wake ordinary-status comparison reported protocol sleep while BLE
was still connected, 572.136 s after first NORMAL. In the synchronized prior
run, the operator saw display-off at9:53 from physical wake, after a 600s
observer had ended. A longer repeat was attempted, but no physical wake was
performed and it closed at its90s no-wake deadline; this adds no standby
measurement. Current evidence is consistent with roughly10min inactivity
sleep. No read-only keepawake query or remote wake has been verified.


A new 5:10 `runpad-w1-discovery-demo` workout sweeps the operator-approved
linear assumption (0% at level0, 11% at level8, 1.375% per level), speed1–6
km/h at level0, and all incline levels up/down at1km/h. It is marked for
simulation and experimental Windows hardware playback. The percentage mapping
is provisional and applies to W1 workout conversion; it is not a measured W1
grade table. Native Linux workout playback remains pending.

The latest idle correlation captured NORMAL at20:19:48.728 UTC after the
operator reported waking the display, then first protocol sleep06/02 at
20:29:43.395 UTC (594.7s later). The follow-up observer began 1:51 after that
reported wake and saw sleep after 483.3s of its own runtime. A preceding trace
captured 3:52 of protocol sleep before the same wake. Raw traces are kept in
the ignored local `docs/treadmill/captures/windows-idle-wake-correlation-*.json`
files. This is the best wake-to-sleep alignment so far, but the physical wake
timestamp came from the operator's message rather than a synchronized button
mark. Polling did not prevent protocol sleep.
