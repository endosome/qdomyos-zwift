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

## Runner semantics

The runner requires three fresh stopped samples before READY and three matching
running samples before timing begins. It generates paired targets, rounds down
to the simulator's explicit resolution, warns 30/10 seconds before stage changes,
and preserves multiple same-offset text events. Stage timing uses supplied
monotonic time; the CLI will use QElapsedTimer. At ramp endpoints it waits for
matching telemetry before the next stage, excluding that settling hold from the
planned active duration. Total wall time therefore includes startup, settling
holds and stop verification.

Pause freezes active time; resume is explicit. Intensity reduction latches lower
ceilings for subsequent stages. Abort cancels countdown/targets through a new
command generation. Missing telemetry, control-loop gaps, rejected commands,
unexpected movement/state changes and stop deadlines prevent successful
completion. The 2/10/15/30-second development deadlines are simulator parameters
and must be calibrated before any hardware backend is enabled.

## Run in simulation

```sh
./treadmill workout list
./treadmill workout run uphill-walk-20 --simulate
./treadmill workout run uphill-walk-20 --simulate --fast --log /tmp/session.jsonl
./treadmill workout run interval-demo --simulate --fast --inject stop-failure
```

Select by ID or explicit XML path. `--workouts-dir` overrides the catalog directory.
Without an override, the installed `../share/treadmill/workouts` directory is used
when present, with the checkout workout directory as a development fallback.
`--fast` advances a deterministic virtual clock and disables stdin controls; it
performs no BLE operations. Normal simulation follows elapsed real time and
accepts P (pause), R (explicit resume), S/Q (stop/quit) and - (reduce intensity).
Terminal state is restored on exit; EOF and SIGINT/SIGTERM/SIGHUP initiate stop.

Each run writes JSON Lines with UTC/monotonic timestamps, requested/reported
pairs, sample age, stages, command submission/acknowledgement, user actions,
connection/running state and final result. By default logs go in the app's
standard local-data `sessions` directory. `--log` requires a new file and never
truncates an existing session. Logs are capped at 32 MiB; recording failure
aborts the run. A successful simulation ends only after observed stopping.

Fault injection: `--inject stale|disconnect|override|stop-failure|reject|no-ack`,
with optional `--inject-at <active-seconds>` (default 5). The no-ack case can still
succeed when telemetry confirms targets; acknowledgements are not physical proof.
Other faults exercise abort/unconfirmed outcomes. A run without `--simulate` is
refused before any Bluetooth discovery or connection because W1 motor capability
and feedback validation is still pending.

Run offline CLI/terminal integration tests after building:

```sh
python3 tools/treadmill/tests/test_cli.py /tmp/treadmill-build/treadmill
```

These exercise both full workouts, fault outcomes, log preservation, EOF,
SIGTERM/terminal restoration and single-key stop. They never invoke BLE scanning
or connect a physical device. Command logs distinguish submitted, acknowledged,
acknowledgement timeout and telemetry-settled phases, with command/session
generation IDs. A missing acknowledgement is recorded without repeating start.

## FitShow reuse and remaining integration

FitShow framing/checksum code is now shared with the existing QZ driver through
`src/devices/fitshowtreadmill/fitshowpacket.h`. Byte fixtures and malformed-frame
cases run in the Core-only test target. Incoming responses require command and
parameter bytes; response lengths no longer truncate to eight bits before
validation. Combined info requires its complete payload before fields are read.
These checks do not establish W1 compatibility.

The full QZ static-library build spike could not configure with the small local
Qt toolchain: Positioning, Quick/QML, NetworkAuth, WebSockets, TextToSpeech,
Location, Multimedia, Charts and QuickControls2 were missing. Driver isolation,
opt-in motor policy, queue cancellation/stop priority and the real controller
adapter still need implementation and full QZ regression builds. The CLI refuses
hardware workout execution until those changes and W1 validation are complete.

## Install and CI

```sh
# Run in the CLI build directory; PREFIX defaults to /usr/local.
qmake /path/to/qdomyos-zwift/tools/treadmill/treadmill.pro PREFIX=/usr/local
make -j2
make INSTALL_ROOT=/tmp/treadmill-stage install
```

Staging installs the binary, external plans, README, status notes and repository
LICENSE. Installation does not bundle Qt or BlueZ; provide the matching dynamic
runtime packages on the target host. The standalone Linux workflow builds the
CLI/Core tests, runs offline simulation/terminal tests without a system Bluetooth
bus, checks staged installation and runs address/undefined-behavior sanitizers.
Full QZ regressions and physical W1 validation remain separate requirements.

See [STATUS.md](STATUS.md) for completed tasks, pending hardware work and the
read-only W1 compatibility checkpoint. Sources remain subject to the repository
license; extracting a library does not change that license.

`./treadmill status --simulate` reports an explicitly labeled simulator snapshot
and synthetic limits without starting movement. Real status queries remain gated
on validated W1 notification/query behavior.

Connection events are logged independently of motor telemetry freshness. A lost
connection immediately revokes automation and marks stopping unconfirmed without
claiming an offline stop command was delivered. Simulator reconnection preserves
possible existing belt movement; new preflight refuses to start in that state.

Diagnostic reports include the Qt version. `info` retains all enumerated service
UUIDs even if later detail discovery fails, and records every characteristic
property flag plus its numeric bits. `address_type` is null because Qt 5's device
information does not expose that fact; it is not guessed from the address.

## Isolated QZ driver build

The bounded driver target compiles and links QZ's FitShow driver, treadmill/base
classes, metrics, settings and virtual-device base with Qt Core, Bluetooth and
Positioning. `TREADMILL_NO_VIRTUAL_DEVICE` excludes peripheral creation from this
separate target; normal QZ builds keep their existing peripheral behavior.
The driver no longer includes GUI headers; QZ's factory delivers its toast signal.

```sh
mkdir -p /tmp/treadmill-driver
cd /tmp/treadmill-driver
qmake /path/to/qdomyos-zwift/tools/treadmill/driver/driver.pro
make -j2
```

This produces an internal static archive, not a stable installed SDK. Building
or linking this target does not connect a device. Positioning development/runtime
packages are an additional dependency (`qtpositioning5-dev` on Ubuntu).

## Restricted FitShow control path

The driver has an opt-in `Mode::Restricted`; legacy QZ callers keep `Mode::Legacy`.
Restricted mode requires an explicit supported GATT mapping, never runs `btinit`,
never creates a virtual peripheral or persists an FTMS switch, and never reconnects
automatically. Notification subscription requires an explicit diagnostics request.
The separate `FitShow::Control` queue requires reported bounds before arming,
serializes commands asynchronously, coalesces unsent target pairs, and purges
pending commands before priority stop. Timed-out motor commands are not retried.
Matching command/subtype replies are labeled weak acknowledgements; full valid
motor status retains raw speed even when status says stopped.

Core-only queue/parser tests use recorded payloads; driver tests link the actual
isolated QZ archive and verify restricted defaults without connecting Bluetooth.
After the archive build, build `driver/tests.pro` with `DRIVER_LIB=<archive-dir>`
and the same optional Google Test include/library overrides as the Core tests.
This is software validation only; it does not approve a W1 command sequence.
