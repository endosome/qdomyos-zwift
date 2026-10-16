# Repository assessment

Planning baseline: commit `8b2e98ccc21cf28d3b946192094f4fb8af40d943`, inspected 2026-10-12. The working tree initially contained untracked `docs/treadmill/TREADMILL.md`. No production implementation, build, BLE connection, or motor test was performed. Line references below refer to this commit and use repository-relative paths.

Evidence labels throughout this document set: **Confirmed** means inspected source behavior; **Inferred** means a consequence of that code, without execution; **Unknown** means hardware or runtime evidence is absent; **Recommendation** means proposed implementation, not an existing API or guarantee.

## Architecture and build

**Confirmed:** `bluetoothdevice` → `treadmill` → `fitshowtreadmill` is the relevant inheritance chain. `src/devices/bluetoothdevice.h:56` exposes metric accessors; `src/devices/bluetoothdevice.cpp:25` implements start/stop by setting request fields. `src/devices/treadmill.cpp:28`, `:75`, and `:117` implement speed, inclination, and paired requests. These functions submit intent; they do not return physical-operation results. Settings apply gains, offsets, limits, rounding, difficulty factors, and external-sensor corrections. Automation is gated by `autoResistanceEnable`. A CLI cannot assume a requested value reaches the wire unchanged.

`src/qdomyos-zwift.pro` and `src/qdomyos-zwift-lib.pro` both include the large `src/qdomyos-zwift.pri`. The latter provides a static archive, not an isolated treadmill library. `src/qdomyos-zwift.pri:2` and `defaults.pri:1` require Bluetooth, Widgets, Quick, XML, Positioning, NetworkAuth, WebSockets, TextToSpeech, Location, Multimedia, Charts and other modules; the include also adds Core private headers, SQL and Concurrent. `src/qdomyos-zwift.pri:26` selects C++17. Linux CI uses Qt 5.15.2 (`.github/workflows/main.yml:465`), with an Ubuntu 24.04 desktop build job at `:404`. Repository test target still declares C++11; align a new target with production C++17.

Directories of interest: `src/devices/` hardware drivers; `src/virtualdevices/` peripheral emulation; `src/characteristics/` standard BLE handlers; `src/trainprogram.*` scheduling; `train-programs-examples/` XML; `tst/` Google Test; `docker/linux/` older build recipe; `.github/workflows/main.yml` platform builds. No W1 support is established by this inspection.

## FitShow discovery and GATT

**Confirmed:** `src/devices/bluetooth.cpp:169` creates a Qt discovery agent. Its device factory selects FitShow at `:3203`–`:3227`, stops discovery, constructs the driver and passes `QBluetoothDeviceInfo` to it. Names include `FS-`, `TR510-T`, `NOBLEPRO CONNECT`, selected `SW` fingerprints, `WINFITA`, `TUNTURI T80-`, `SW-BLE`, and `BF70`, with settings/service exclusions. Earlier `FS-` cases exist at `:1421`, `:2081`, `:2196`, `:3158`, and `:3172`. Selection order and settings matter; a W1 name must not be guessed or added without conflict checks. A dedicated CLI should select the explicitly configured device and validated GATT layout instead of constructing the entire factory.

Driver central connection: `src/devices/fitshowtreadmill/fitshowtreadmill.cpp:895` (`deviceDiscovered`). Connection triggers service discovery; `serviceScanDone` at `:852` constructs the selected service and optionally RSC service `1814`.

| Candidate service | Write characteristic | Notify characteristic |
|---|---|---|
| `FFE0` | `FFE1` | `FFE4` |
| `FFF0` | `FFF2` | `FFF1` |
| `AE00` | `AE01` | `AE02` |

These are Bluetooth-base UUID short forms. **Confirmed:** `serviceDiscovered` at `:293` lets `FFF0` overwrite a prior selection, while `FFE0` and `AE00` are selected only if no service was selected. `stateChanged` at `:754` searches characteristic IDs independently within that selected service; it does not validate a strict mapping table. It subscribes via CCCD `01 00`. `1814/2A53` supplies optional cadence. Descriptor completion sets `initRequest` and emits `connectedAndDiscovered` (`:839`), before capability responses arrive. Missing optional RSC returns from service discovery after communication-service discovery was started; verify notification initialization when RSC is absent.

Presence of FTMS `1826` may persist `ftms_treadmill` and request an app restart, except for certain name flags (`:302`). That mutation and `homeform` dependency are inappropriate for a dedicated controller without an explicit policy.

## Packets, units and controls

**Confirmed:** `writePayload` (`:111`) encodes `02 | payload | XOR(payload) | 03`, at most 25 bytes total (22 payload bytes). `checkIncomingPacket` (`:93`) checks delimiters and XOR, minimum frame length four. `characteristicChanged` (`:318`) treats each non-RSC notification as one complete FitShow frame. There is no streaming fragment assembler. A four-byte valid envelope has only one payload byte, yet parser reads command and parameter. Several branch length checks also merit review, notably combined info subtype 5 reading fields through index 8 with `full_len >= 9`, which can include checksum/footer rather than a complete payload. Strict per-message lengths and integer-safe indexing are required before reuse for automation.

Command IDs/constants are in `fitshowtreadmill.h:36`–`:77`: info `50`, status `51`, data `52`, control `53`, key `54` in hexadecimal. Control subtype `01` starts/readies normal sport mode, `02` sets targets, `03` stops, `06` pauses. Pause and start packet comments explicitly say “to verify.”

`forceSpeedOrIncline` (`:128`) transmits speed and incline together: payload `[53,02,speedByte,inclineByte]`. Speed is divided by 1.60934 only when `fitshow_treadmill_miles` is enabled, multiplied by ten, clamped against private protocol min/max, and rounded to a byte. Incline is clamped and encoded as integer, doubled for NoblePro. NoblePro reports half-unit steps; default FitShow step is one (`.h:153`, `.cpp:904`, `:980`). These are QZ conventions, not verified W1 resolution. Default max speed 300 exceeds a single byte; defaults must never be treated as hardware discovery.

Info responses (`:377`) parse speed min/max and optional `UNIT`, incline min/max and pause flag, model identifier, factory date, total usage, or AnyRun combined info. `UNIT` is stored privately but does not select speed conversion; a setting does. Comments claiming miles “always” conflict with this conditional code. Unit verification is a hardware gate.

Start is queued with the last remembered speed/incline, falling back to 0.5 speed (`update`, `:222`). `tapeStarted` is emitted immediately after enqueueing (`:245`), so it is not movement confirmation. Stop/pause is appended after target and start processing (`:247`). Pause depends on `paused`, whereas base `stop(true)` sets `requestPause`; callers also manage pause state. Do not equate the existence of the constant or flag with verified pause/resume.

`btinit` (`:693`) queues user data, a dated model request and speed/incline/total/date info requests; AnyRun uses a different sequence. It sets `initDone` on enqueue, not successful discovery. `atletica_lightspeed_treadmill` can pass `startTape=true` from `update:165`, enqueueing start and targets during initialization. An unmodified driver is therefore unsuitable for initial read-only diagnostics.

## Queue, acknowledgements and telemetry

**Confirmed:** `scheduleWrite` (`:57`) appends length-prefixed payloads and debug strings. `update:262` sends the front entry on odd retry counters, waits on even ones and discards it after counter six (three attempts). At default 200 ms refresh, attempts nominally occur every two ticks; nested waits change actual timing. When empty it polls status. There is no stop priority, command ID, public queue cancellation, or surfaced timeout result.

`writeCharacteristic:63` enters a nested `QEventLoop` awaiting any `characteristicWritten` or a 300 ms single-shot. It may use WriteWithoutResponse. **Inferred:** nested event processing permits reentrancy; the timeout diagnostic uses a timer object whose static singleShot does not make that object active, so it cannot reliably classify completion. Transport callbacks do not mean accepted motor commands.

Incoming non-status frames remove the queued head when command and parameter match (`:372`–`:376`); target bytes are not compared. A delayed response to an older target can match a newer target. Control target responses only log apparent speed/incline (`:433`); they do not establish settled telemetry.

Status handling (`:447`) recognizes countdown/start, running, stop, paused, end, study, error and safety. Full running/stop/paused/end reports of at least 16 bytes update `IS_RUNNING`, speed byte/10, signed incline, elapsed time, distance, calories, steps and heart. AnyRun changes byte ordering/time interpretation. NoblePro halves incline. `parseSpeed` and inclination overrides transform metrics (`:532`, `:547`). Nonrunning reports force public speed zero (`:532`), potentially hiding physical deceleration. Raw frame fields and status must be exposed separately for stop observation. Error/safety flags do not themselves create a CLI stop policy.

`packetReceived` is emitted before checksum validation and also for RSC (`:333`): it cannot measure valid motor telemetry freshness. Capabilities/status flags are private (`.h:112`–`:137`); existing speed/incline change signals are conditional and unsuitable as a heartbeat. `connected()` only means controller services discovered (`:948`); `controllerStateChanged:957` automatically reconnects and resets initialization. Queued commands are not explicitly purged there.

## Workouts and headless behavior

**Confirmed:** `trainrow` (`src/trainprogram.h:24`) contains duration, distance, speed, incline, force-speed flag, ranges, ramp bookkeeping and text events. `loadXML` (`src/trainprogram.cpp:1961`) accepts `<rows><row .../></rows>`, `<repeat times="...">`, `duration="hh:mm:ss"`, `speed`, `inclination`, `forcespeed`, and speed ramps `speedfrom/speedto`. Power ramps also exist. There is no incline-ramp or stage-name/description/unit parser in the inspected XML loader. `minspeed/maxspeed` also serve automatic intensity/HR machinery; their names alone do not establish generic interval range behavior.

Speed ramps expand into rows in nominal 0.1 increments (`:2108`); the loop uses `i < speedDelta`, so exact terminal target and distributed duration must be tested. Ramp child text events are skipped by the non-ramp-only loader branch (`:2278`). Repeats use a single working repeat buffer; nested repeat semantics are not established. Text events use row-relative seconds and `toastRequest` (`:1283`); deduplication key is row index plus time offset, so same-offset messages collide. Events and jitter correction use wall-clock `QDateTime`, not monotonic time (`:644`, `:909`). End optionally emits stop according to `trainprogram_stop_at_end`, without verification (`:1315`), and GPX circuit behavior can restart.

Scheduler receives `bluetooth*`, inspects concrete device types and includes OCR, GPX, HR/power and platform integrations. Constructor can initialize Zwift authentication from settings (`:38`). It starts a one-second timer (`:82`). `onTapeStarted:1666` simply marks it started; existing driver signal is enqueue-time. Reuse XML semantics and suitable parsing code; running this whole scheduler unchanged would need substantial safety and isolation work.

`src/main.cpp:300` handles `-no-gui`; `:444` constructs `QCoreApplication`. `-train` is parsed at `:370`, but used to construct `MainWindow` only in the `QApplication` branch (`:1069`–`:1080`). `src/mainwindow.cpp:43`, `:293`, `:370` load and wire the program. **Confirmed source gap:** the inspected non-GUI branch is empty; `-no-gui -train file` does not wire that workout. **Inferred:** BLE bridging can operate headlessly, but unattended safe workout execution is not supplied by these flags. No runtime headless claim was tested.

## Logging, tests and dependency limits

QZ uses `QSettings` keys/defaults from `src/qzsettings.*`, driver `debug` signals and `myMessageOutput` (`src/main.cpp:500`). On Linux non-QML logging is controlled by `logs`; warnings can be suppressed. A separate CLI should emit faults regardless of debug logging and use a separate settings namespace rather than importing the user's QZ profile.

`tst/qdomyos-zwift-tests.pro` links the full static QZ library and uses the Google Test submodule; `.gitmodules` records release-1.12.1. FitShow detection cases are in `tst/Devices/devicetestdataindex.cpp:412`, `:461`, `:467`, `:489`. `tst/TrainingProgram/trainprogramtestsuite.cpp` covers transition barriers. These establish test patterns, not W1 motor compatibility or a complete protocol/safety suite.

A new pure workout/simulator target can use Qt Core and C++17. A truly trimmed hardware target would need Core, Bluetooth, and Positioning through `metric` → `sessionline` (`src/sessionline.h:5`), plus actual transitive link dependencies. Existing FitShow includes `homeform.h`, virtual treadmill and inherited metric/settings code. **Unknown:** exact minimal link closure until a build spike. Linking the existing static library first maximizes reuse but retains broad build dependencies and may pull more objects. Do not promise Core/Bluetooth-only driver reuse.

## Linux, packaging and licenses

Recommendation: use a pinned Ubuntu 24.04/Qt 5.15 build matching existing CI, qmake shadow builds, and initially the existing QZ dependency recipe. `docker/linux/Dockerfile` uses `ubuntu:latest` and `qt5-default`; treat it as historical evidence, not a reproducible current recipe. No packages were installed or builds attempted here.

Qt 5.15 supports BLE central on Linux, requires a functional BlueZ/D-Bus backend, and can otherwise select a dummy backend: [Qt Bluetooth 5.15 documentation](https://doc.qt.io/archives/qt-5.15/qtbluetooth-index.html). Plan a running BlueZ service, system bus access, powered unblocked adapter and actual BLE permissions checks. Root is not the default deployment model; determine distro D-Bus/polkit policy and any backend-specific capability requirement in M1. Containers need explicit host bus/adapter access for manual hardware tests; CI uses simulation.

Repository `LICENSE` contains GPL version 3. Library extraction does not grant relicensing rights; retain applicable notices/source availability when distributing derivatives, following [GNU guidance on GPL libraries](https://www.gnu.org/licenses/gpl-faq.en.html#IfLibraryIsGPL). `src/qmdnsengine/LICENSE.txt` is MIT; `.gitmodules` includes SMTP, Google Test, HTTP server, OpenSSL and ZwiftPlay components. Inventory the components actually linked before release, including Qt module licenses and bundled/generated SDK code. This is a packaging requirement, not a completed legal audit. Ship the CLI, example XML, schema/config documentation, notices and matching source/build instructions; defer distro packages until link/runtime closure is proven.
