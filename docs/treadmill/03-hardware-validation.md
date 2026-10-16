# W1 hardware validation

Baseline: [source analysis](01-repository-analysis.md). **No physical W1 was accessed.** FitShow app compatibility, eight motorized incline settings and approximately 11% maximum incline are reported in the task, not confirmed here. Exact model variant and firmware are unknown.

## Gates and safe prerequisites

Gate 1 is strict read-only discovery: advertisements, service/characteristic/descriptors metadata and readable characteristics. No FitShow driver construction, characteristic writes, pairing/reset operations or notification subscription: CCCD subscription itself writes. Record unavailable values as unknown. Connecting may change controller ownership even without writes; observe console behavior and disconnect if unexpected.

Gate 2 is an explicitly reviewed diagnostic mode allowing only named nonmotor operations, including notification subscription and individually approved information/status queries. It is a separate step because FitShow capabilities may require writes; successful read-only GATT discovery cannot complete capability validation. Do not replay `btinit`: it writes user/date data and may start with certain QZ settings. Capture exact bytes and verify their interpretation before use. If capabilities require unverified commands, stop at the evidence gap and investigate official-app traces.

Gate 3 permits motor tests only after identity, limits, units and a safe initial state are validated and the operator explicitly confirms the proposed test. If protocol does not expose limits, a device-specific profile requires exact manufacturer/model evidence plus physical validation; missing/inconsistent evidence keeps automatic workouts disabled. Never use QZ default limits as evidence. One test confirmation applies to its stated bounds, not future arbitrary workloads.

Before Gate 3: accessible tested physical stop/safety mechanism, clear belt/space, operator at physical controls, manufacturer operating instructions available, conservative explicit ceilings, no simultaneous FitShow/QZ controller, stable Linux adapter and complete logging. Validate initial motion testing without a person on the belt where manufacturer procedure permits. Do not defeat safety-key mechanisms to obtain telemetry.

## Ordered diagnostic workflow

1. Record Linux distribution, kernel, Qt/BlueZ versions, adapter and address type, QZ revision; inspect service/adapter permissions without changing global permissions or running everything as root.
2. Scan advertisements in read-only mode. Record local name, address/address type, manufacturer/service data, advertised UUIDs and RSSI; do not treat a name or RSSI as unique identity. Repeat after power cycle to check identity stability. Select explicitly if multiple candidates exist.
3. Connect using bare Qt central. Discover all GATT services and properties. Record full UUIDs, handles, read/write/notify/indicate flags, CCCDs and security errors. Read Device Information/model/firmware only if exposed and readable.
4. Compare `FFE0/FFE1/FFE4`, `FFF0/FFF2/FFF1`, `AE00/AE01/AE02` and optional `1814/2A53`; identify FTMS `1826` without enabling QZ's FTMS persistence. Multiple candidates require explicit evidence-based selection; prefer neither by guessed enumeration order.
5. Save a read-only report and unresolved fields. If no known mapping exists, classify unsupported pending traces; do not send probe motor packets.
6. At Gate 2 subscribe to the validated notify characteristic, observe console-controlled sessions and capture reports. Query only reviewed info/status operations if needed. Determine initialization, pairing/authentication, fragmentation, polling and acknowledgement behavior. Passive observation first; official-app capture can reveal initialization without speculative replay.
7. Establish speed and incline evidence as below. Review all limits and stop behavior; enter Gate 3 separately for bounded start/stop tests. Never make a full workout the first motor test.

[BlueZ GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/) distinguishes reads, writes and notification methods; read-only here deliberately excludes subscriptions even when their application purpose is observation.

## Calibration procedure

Speed: compare raw frame byte, console units and official-app units at several low console-selected speeds while observing physical behavior. Test whether speedByte/10 is km/h or mph and whether optional UNIT agrees. If available, use independent belt-speed measurement by a manufacturer-approved method; console agreement alone does not prove actual belt speed. Check minimum running speed, resolution, requested-versus-reported behavior and deceleration reports. Discover maximum from reliable documentation/protocol without driving at maximum. Retain the most restrictive verified limits, reject conflicting evidence, and cap initial tests below them. Log km/h externally, retain raw bytes internally.

Incline: do not assume eight settings mean eight percent or equally spaced levels. Measure deck angle safely at each console-selected supported setting, including baseline; convert to percent grade as `100 * tan(angle)`, not degrees. Record raw code, console setting, measured grade, uncertainty, ascent/descent delay and hysteresis. Determine whether telemetry reports a setpoint or attained position. Calibrate both directions. If no physical-position feedback exists, label reports accordingly; elapsed waiting alone cannot establish attainment. Reject unmapped grades or use an explicitly previewed conservative floor mapping; never silently round upward. No automatic incline workout without adequate validated observation/mapping.

Start/stop: with confirmed initial stopped state and a fresh raw sample, confirm a bounded start using verified minimum speed/flat position. Capture countdown and transition into running. Independently observe movement versus status; check whether reports zero speed before belt stops. Request stop, capture transport completion/protocol echo/status/deceleration and physical stop. Repeat only after reviewing results. Verify stop idempotence, latency and pause flag/behavior separately. Pause/resume remains disabled until tested; stop-then-start is not assumed equivalent.

## Validation matrix

| Topic | Confirmed source | Physical evidence needed / unknown | Pass gate |
|---|---|---|---|
| W1 identity | QZ name patterns only | Exact variant, name, stable address/identity, firmware | Selected identity remains unambiguous |
| GATT | Three candidate UUID families | Actual family, properties, security and optional RSC absence | Validated single mapping |
| Protocol frames | `02 payload XOR 03` | Same framing, lengths, fragments/coalescing | Captured samples parse without guessed fields |
| Initialization | QZ sends user/date/info; AnyRun variant | Required sequence, authentication, query side effects | Reviewed nonmotor initialization |
| Speed limits/units | Private limit parser; setting conversion | Actual limits, resolution, console/byte/unit relationship | Conservative verified envelope |
| Incline | Integer code; NoblePro half step | Eight settings claim, percent mapping, attained-position feedback | Calibrated table and known observation semantics |
| Start | `53/01` then combined target | Countdown, initial target, actual movement | No unexpected initial acceleration |
| Stop | `53/03` appended to queue | Idempotence, deceleration, physical stop observability | Bounded latency and valid stop evidence |
| Pause | `53/06`, flag parsed | Supported semantics and resume procedure | Enable only if independently passed |
| ACK | Command/subtype match | Echo vs acceptance, rejection, late replies | Correlation limitations documented |
| Status | Running/stopped/paused/error/safety parser | Rate, raw zero semantics, error codes, reported vs measured values | Fresh relevant samples usable |
| Ownership | No exclusivity guarantee inspected | Official app/second-controller interactions | Operational single-controller procedure |
| Link loss/reset | Driver reconnects | Device behavior with lost controller/safety key/reset | No automated restart; operator understands stop limitation |

## Evidence and exit criteria

For each test store procedure, raw trace, console/physical observation, expected and actual result, model/firmware, time, units, operator confirmation and unresolved discrepancies. Do not record user health data. Communication-loss tests first run in simulation, then manually at low intensity with physical-stop access; do not intentionally remove control during a populated high-speed session.

M1 passes when start/stop/speed/incline and observation requirements have concrete evidence, or unsupported functions are explicitly classified. If incline cannot be observed adequately, deliver diagnostics and simulation and record automatic incline execution as blocked; do not silently substitute reported setpoint for attained grade. Full W1 workout approval is a later gate after controller/simulator safety tests pass.
