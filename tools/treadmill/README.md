# treadmill CLI

Initial implementation: read-only BLE discovery on Linux. This standalone target
uses Qt Core and Bluetooth without loading QZ's device factory or FitShow driver.

## Build

Requires a C++17 compiler, make, qmake and Qt 5.15 Core/Bluetooth development
packages. Debian/Ubuntu packages include `build-essential`, `qtbase5-dev`,
`qt5-qmake` and `qtconnectivity5-dev`.

Build outside the source directory (adjust the checkout path as needed):

```sh
mkdir -p /tmp/treadmill-build
cd /tmp/treadmill-build
qmake /home/mix/qdomyos-zwift/tools/treadmill/treadmill.pro
make -j2
```

This target does not include QZ's full application project or require translation
artifacts.

## Scan

```sh
./treadmill scan
./treadmill scan --timeout 15
```

Scans BLE devices for 10 seconds by default; permitted range is 1–60 seconds.
JSON output contains names, addresses, RSSI and advertised service UUIDs, sorted
by address. These observations do not verify identity, GATT capabilities or
FitShow compatibility. An empty advertised UUID list does not establish that a
device has no services.

A watchdog reports failure if discovery does not finish within one second of the
requested duration. Partial results are retained. Exit status is 0 for completed
discovery (including no devices), 1 for discovery failure/watchdog timeout, and
2 for invalid command or timeout values. Parser errors such as unknown options
use Qt's nonzero exit status.

The scanner makes no GATT connections, characteristic writes or notification
subscriptions. Motor controls are not implemented.

Linux needs a functional BLE adapter, running BlueZ and system D-Bus access.
Check adapter power, rfkill and distribution permissions on discovery failure.
Qt needs a real Linux Bluetooth backend; a dummy backend cannot scan. Running
as root does not fix a missing backend.

## Current validation status

Built with Qt 5.15.18 using a local toolchain. Help and invalid-argument paths
were exercised without accessing Bluetooth. Physical discovery remains unverified.
No physical-device actions were performed during implementation.

Next planned task: explicit-device read-only GATT metadata inspection, before
subscriptions, protocol queries or motor-control implementation.

## Read-only GATT inspection

```sh
./treadmill info --device AA:BB:CC:DD:EE:FF --timeout 10
```

Select an address from `scan`. The command scans for that exact BLE address,
connects and reports service UUIDs, characteristic properties/handles and
CCCD/other descriptor metadata. Qt 5.15 detail discovery can read characteristic
and descriptor values; this command issues no GATT writes or subscriptions.
Standard readable model, firmware, hardware and manufacturer information is
shown when available. Motor capabilities remain explicitly unknown. The
connection/detail-discovery phase has a total 30-second deadline and returns
partial metadata on error. Actual W1 GATT inspection remains hardware-unverified.

## Controller and simulation model

`Controller` defines asynchronous requests and separate telemetry/command events.
`Simulator` implements countdown, acknowledgement delay, speed/grade response,
priority stop with generation cancellation, pause, and injected failures. Its
0.5–6 km/h and 0–10% limits are simulation parameters, not W1 specifications.
A submitted or acknowledged operation does not establish motion or stopping.

The Core-only Google Test target has no Bluetooth dependency:

```sh
mkdir -p /tmp/treadmill-tests
cd /tmp/treadmill-tests
qmake /home/mix/qdomyos-zwift/tools/treadmill/tests/tests.pro
make -j2
./treadmill-tests
```

Requires Google Test development headers/library (`libgtest-dev` on Ubuntu).
For a nonstandard installation, pass `GTEST_INCLUDE=<path>` and `GTEST_LIB=<path>`
to qmake. Tests exercise acknowledgement versus movement, countdown cancellation,
stale-generation rejection, stop failure and capability validation.

## Workout files

The loader retains QZ's `rows`, `row`, `repeat`, `speedfrom/speedto`, `inclination`
and `textevent` syntax. The CLI dialect requires root `version="1"`, `id`, `name`,
`speedunit="km/h"` and `inclineunit="percent"`; each row requires a name,
positive `hh:mm:ss` duration, `forcespeed="1"` and explicit targets. Incline ramps
use `inclinationfrom/inclinationto`. These additions are not implemented by the
ordinary QZ workout loader; legacy files need explicit metadata before CLI use.
Ramps retain child events and exact endpoints. Unsupported controls, nonfinite
values, nested repeats and excessive expansion are rejected.

`workouts/uphill-walk-20.xml` provides 1,200 seconds of editable uphill walking.
`workouts/interval-demo.xml` provides an unrelated 120-second repeat/ramp example.
Their speed/grade values are prototype targets, not personal prescriptions or
verified W1 capability claims. Device adaptation must happen before a real run.
