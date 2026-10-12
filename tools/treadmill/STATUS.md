# Implementation status

This project is not yet a validated W1 motor controller. Hardware execution is
explicitly disabled. Hardware testing is postponed at the user's request.
Read-only discovery, approved nonmotor diagnostics, real-adapter software and
complete simulation are implemented.

| Roadmap task | Status | Evidence / next dependency |
|---|---|---|
| M0 assessment | Complete | Eight local planning documents under ignored docs/treadmill |
| 1.1 standalone scan | Implemented; physical scan unverified | Qt 5.15.18 build and argument validation |
| 1.2 read-only info | Implemented; physical GATT unverified | Bounded explicit-address inspection, no write/subscription calls |
| 1.3 exact W1 identity | Pending physical access | First operator checkpoint: scan/info reports |
| 1.4 QZ link spike | Isolated driver compiled and linked | Seven QZ sources plus the adapter; Core/Bluetooth/Positioning; no GUI or virtual peripheral |
| 1.5 nonmotor probe | Implemented; not run on hardware | Profile approval + typed confirmation; raw trace only, never armed |
| 1.6–1.7 calibration/start-stop | Postponed physical validation | No W1 units/mapping/feedback facts assumed |
| 2.1–2.3 controller/simulator | Implemented | Core-only deterministic and failure tests |
| 2.4–2.7 restricted driver policy/queue/parser | Implemented; hardware unverified | Offline queue/parser tests and actual isolated-driver tests |
| 2.8 real adapter/profile validation | Implemented; physical acceptance pending | Offline injected-frame tests; actual Qt binding compiled/linked; live identity/bounds/evidence gates |
| M3 workout engine | Implemented for simulator | Generic XML, ramps/repeats/events; external 20-minute and second plan |
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

No physical treadmill, motor command, Bluetooth connection or scanning action was
run during this implementation session. Existing device support must not be
claimed from simulation or byte fixtures.

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

Latest offline results: 40 Core tests, 4 isolated-driver tests and 11 CLI/terminal
tests pass. Core tests pass under address/undefined-behavior sanitizers. The
selected existing QZ regressions have 32 passes and 3 documented skips. The
adapter independently latches faults and revokes queued motor authorization when
reports become stale; fresh packets cannot silently resume a faulted session.

The complete external 20-minute workout also passes through the production
FitShow queue, raw decoder and adapter using a delayed synthetic peripheral.
Startup waits for asynchronous capabilities/identity and three stopped reports;
missing initial reports expire the bounded preflight without arming. Priority
abort overtakes a delayed target reply and never transmits another motor target.
