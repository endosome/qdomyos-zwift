# Implementation status

This project is not yet a validated W1 motor controller. Hardware execution is
explicitly disabled. Read-only hardware discovery has now started at the user's request.
Read-only discovery, approved nonmotor diagnostics, real-adapter software and
complete simulation are implemented.

| Roadmap task | Status | Evidence / next dependency |
|---|---|---|
| M0 assessment | Complete | Eight local planning documents under ignored docs/treadmill |
| 1.1 standalone scan | Implemented; physical scan unverified | Qt 5.15.18 build and argument validation |
| 1.2 read-only info | Implemented; physical GATT unverified | Bounded explicit-address inspection, no write/subscription calls |
| 1.3 exact W1 identity | Operator confirmed selected treadmill; BLE module identified | FITSHOW FS-BT-C1, hardware 1.0, software 1.3.8; firmware revision not exposed |
| 1.4 QZ link spike | Isolated driver compiled and linked | Seven QZ sources plus the adapter; Core/Bluetooth/Positioning; no GUI or virtual peripheral |
| 1.5 nonmotor probe | Seven Windows probes completed after explicit approval | Bounds valid; latest observer captured RUNNING/STOP frames during the repeated manual test; a prior session showed SAFETY throughout; no motor commands |
| 1.6–1.7 calibration/start-stop | Manual physical start/pause captured; software control and calibration unverified | Raw speed code 10, incline 0; pause frames decay 10→7→4→1, then NORMAL. Earlier display reading 1.0→0.0 lacks a reported unit and was not repeated for latest capture; raw zero unknown |
| 2.1–2.3 controller/simulator | Implemented | Core-only deterministic and failure tests |
| 2.4–2.7 restricted driver policy/queue/parser | Implemented; hardware unverified | Offline queue/parser tests and actual isolated-driver tests |
| 2.8 real adapter/profile validation | Implemented; physical acceptance pending | Offline injected-frame tests; actual Qt binding compiled/linked; live identity/bounds/evidence gates |
| M3 workout engine | Implemented offline | Simulator and synthetic FitShow full-session tests; generic XML, ramps/repeats/events |
| M4 CLI | Simulation complete; real run pending | Catalog, controls, logs, shutdown and offline integration tests |
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
