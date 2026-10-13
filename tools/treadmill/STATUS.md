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
| 1.5 nonmotor probe | Two Windows probes completed after explicit approval | Valid bounds; SAFETY changed to NORMAL after normal magnet attachment; no motor commands |
| 1.6–1.7 calibration/start-stop | Postponed physical validation | No W1 units/mapping/feedback facts assumed |
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
as `0251005103`. Both probes cleaned up notifications successfully. The paired
observations support the missing magnet as the cause of the earlier safety state;
sleep behavior has not been established. NORMAL is a short status-only reply:
it contains no speed/incline feedback and does not certify physical stopping.
These raw replies do not validate speed units, grade mapping, pause, stop or
physical movement. Two regression tests use the sanitized observed frames to
check decoding and revocation of queued control on the safety response.
