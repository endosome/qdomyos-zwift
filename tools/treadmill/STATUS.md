# Implementation status

This project is not yet a validated W1 motor controller. Hardware execution is
explicitly disabled. Read-only BLE discovery/GATT inspection and complete
simulation are implemented.

| Roadmap task | Status | Evidence / next dependency |
|---|---|---|
| M0 assessment | Complete | Eight local planning documents under ignored docs/treadmill |
| 1.1 standalone scan | Implemented; physical scan unverified | Qt 5.15.18 build and argument validation |
| 1.2 read-only info | Implemented; physical GATT unverified | Bounded explicit-address inspection, no write/subscription calls |
| 1.3 exact W1 identity | Pending physical access | First operator checkpoint: scan/info reports |
| 1.4 QZ link spike | Isolated driver compiled and linked | Six QZ sources; Core/Bluetooth/Positioning; no GUI or virtual peripheral |
| 1.5–1.7 nonmotor queries/calibration/start-stop | Pending evidence and operator confirmation | Do not replay driver initialization or issue motor probes |
| 2.1–2.3 controller/simulator | Implemented | Core-only deterministic and failure tests |
| 2.4–2.8 real driver policy/adapter | Pending | QZ build environment, validated profile; queue/reconnect/raw-feedback changes required |
| M3 workout engine | Implemented for simulator | Generic XML, ramps/repeats/events; external 20-minute and second plan |
| M4 CLI | Simulation complete; real run pending | Catalog, controls, logs, shutdown and offline integration tests |
| M5 hardening/package | Initial support implemented; release acceptance pending | Linux CI/install targets; W1 and full QZ regressions not validated |

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

After reviewing GATT evidence, implement a restricted FitShow adapter and
nonmotor query/subscription mode. That is a separately reviewed write-enabled
step, not part of the read-only tool. Establish actual protocol framing,
initialization, speed units/limits and discrete incline mapping before motor use.
Only then propose a bounded minimum-speed start/stop test with explicit operator
confirmation and physical-stop access. A full workout is the final hardware
checkpoint after simulator safety tests and control/feedback validation.

## Current implementation boundaries

Simulator capabilities and timing limits are synthetic. It can confirm simulated
movement/stopping, not W1 movement. The future hardware controller must expose
raw valid feedback and calibrated discrete grade mapping rather than infer them
from QZ public metrics. Pausing is implemented/verified only in simulation.

The existing QZ driver now shares its pure packet framing helper. Its default
FIFO stop ordering, automatic reconnection/initialization and private capability
state still require opt-in CLI changes. The CLI never constructs that driver.
The isolated QZ driver compiles and links; full application/regression builds are separate checks.

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
