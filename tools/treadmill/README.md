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

Compilation/runtime discovery have not been verified: qmake and Qt5Core/
Qt5Bluetooth development metadata were unavailable in the development environment.
No physical-device actions were performed during implementation.

Next planned task: explicit-device read-only GATT metadata inspection, before
subscriptions, protocol queries or motor-control implementation.
