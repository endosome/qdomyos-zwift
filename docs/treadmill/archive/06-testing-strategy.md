# Testing strategy

This document plans future tests; none were implemented or executed during repository assessment. Baseline and existing tests: [01](01-repository-analysis.md). Hardware gates: [03](03-hardware-validation.md). Required invariants: [05](05-safety-and-state-machine.md).

## Test boundaries

Use the existing Google Test/Google Mock conventions where practical. New runner/simulator tests should link only their own Core-based target, rather than pull in BLE/UI through `tst/qdomyos-zwift-tests.pro`. Existing QZ driver regression tests can use the current static-library test target. Pin the Google Test submodule revision used by CI and compile new tests as C++17. Avoid globally installing a different framework.

Inject a monotonic clock and a recording asynchronous controller into the runner. Advance simulated time deterministically; do not wait 20 real minutes or sleep to test deadlines. Tests compare externally visible state/events/commands, not private implementation structure. Simulator and fake transport expose only the hooks necessary for meaningful failure scenarios.

## Simulator contract

Implement the same QObject controller slots/signals and capability/telemetry/result data as real hardware. Internally track connection, raw status, actual speed/grade, target pair, supported limits, movement/countdown, separate belt/incline response delays and acknowledgement delay. A virtual-clock step updates physical model and emits valid timestamped telemetry. Start/stop may be acknowledged before or after physical effects. Profiles expose configurable speed resolution, discrete grade mappings and reported-setpoint-only modes.

Inject disconnect, rejected command, lost/late/duplicate ack, malformed/missing telemetry, per-field stale data while connection/other fields remain fresh, unexpected running/safety/error state, physical speed/grade override, refused stop, slow deceleration and stop ACK with continued movement. Reconnection never applies retained targets. Test simulator itself against controller contract so favorable simulator behavior does not conceal adapter bugs. Hardware backend factory must not be constructed under `--simulate`, even when real Bluetooth devices are present.

## Test matrix and acceptance

| Category | Cases | Acceptance evidence |
|---|---|---|
| Protocol encoding | Info/status/start/stop/pause/paired payloads; XOR; max lengths; unit scaling; rounding/bounds | Byte-exact source-derived fixtures and later captured W1 fixtures, labeled separately |
| Protocol decoding | Each recognized info/status/data subtype; boundary/short frames; wrong delimiter/XOR; signed incline; AnyRun endian/time; NoblePro scaling; RSC separation; oversized data | No out-of-bounds reads, invalid data never updates freshness/capabilities |
| Message delivery | Weak subtype ACK, stale same-subtype reply, write-no-response callback absence, notification fragments/coalescing where evidenced | No falsely settled command; transport behavior explicitly classified |
| Capabilities/profile | Missing/contradictory limits; default values; unknown unit; firmware mismatch; absent optional RSC; multiple GATT families | Motor execution disabled unless validated envelope and layout exist |
| XML loader | Existing fixed rows/repeats/speed ramps/events; proposed metadata/incline ramps; malformed XML/numbers; nested/huge repeats; durations; duplicate IDs; unsupported controls | Clear errors, bounded input, stable round trip where supported |
| Runner | Ordered stages; 1,200-second example; second unrelated workout; exact ramp endpoints; quantization; lag; pause; user reductions | Generic behavior independent of identifier/discipline; no endpoint/duration drift |
| Notifications | 30/10/0 seconds; repeated occurrences; same-offset events; ramp children; short stages; delayed tick | Correct targets and names, once per occurrence, no obsolete warning replay |
| Timing | Wall-clock changes, skipped wakes, maximum gap, OS resume, fake monotonic clock | No wall-clock dependence or catch-up acceleration burst |
| Safety | Abort during speed/incline write, queued start, delayed callback, stale data, lost ACK, console intervention, link loss, stop failure | All invariants below pass for every ordering tested |
| CLI integration | List/run/simulate; ambiguous device; invalid profile/file; terminal input/EOF; signal exit; noninteractive real start rejection | Correct command routing, visible faults, bounded shutdown and outcome log |
| QZ compatibility | Existing detection/settings/workout tests; default constructor policy; legacy XML loader behavior | Opt-in CLI changes do not alter ordinary QZ behavior |

Run sanitizer builds on the protocol/loader/state tests where available. Add fuzz/property coverage for packet lengths/checksums and XML numeric boundaries after deterministic fixtures, focusing on parser invariants rather than an unfocused new fuzzing infrastructure. Source fixtures show QZ behavior; only physical traces justify W1 compatibility.

## Mandatory safety invariants

1. No motor write before validated capabilities, fresh initial state and explicit start authorization.
2. Stop/abort invalidates every pending start/target and retry; no later acceleration from old generation callbacks.
3. Disconnect revokes authorization immediately, never reconnects/resumes motor control automatically.
4. Acknowledgement or synthetic metric zero alone never reports physical target completion/stop.
5. Stale or malformed reports cannot refresh motor field ages; RSC cadence and arbitrary notifications do not count as status.
6. User reduction ceilings persist across later stages; physical stop/safety event cannot trigger restart.
7. Paused time does not advance active elapsed; resume requires verified hardware support and explicit confirmation.
8. Stop deadline failure yields STOP_UNCONFIRMED with physical-stop warning and nonzero outcome.
9. Simulator execution makes zero BLE discovery/connection/write calls.

Exercise critical orderings explicitly: abort before/after submit, ACK before/after telemetry, disconnect before/after stop submit, old write callback after a new generation, timer transition concurrent with stop input, telemetry during initialization, terminal EOF during countdown, and firmware reset while running. Use transport recordings to assert actual write order; runner-only tests cannot validate driver FIFO purge.

## Integration and CI plan

Build CLI/runner/simulator first in a pinned Linux/Qt environment. Run pure tests and a deterministic full example plus second-plan simulation without system bus, adapter or display. Record final target trace, transition events, summed active duration, complete stop outcome and JSON Lines parseability. Short pseudoterminal tests cover single-key controls, terminal restoration and EOF. Subprocess signal tests use simulated hardware only.

Driver integration uses a fake GATT boundary where feasible and tests initialization policy, optional services, per-message freshness, write modes, cancellation and reconnect policy. If Qt service objects cannot be usefully faked, introduce a small internal transport seam after proving the need; do not pretend mock runner tests cover actual BLE behavior. Existing QZ library builds/regression tests are a separate CI job for any source changes. CI must never enable the real backend automatically or run motor tests from environment-detected adapters.

Hardware tests are manually invoked, isolated from CI and named distinctly. Require selected identity/profile, recorded confirmation and bounded procedure. Sequence: read-only GATT report → reviewed nonmotor subscription/query → console-driven unit/mapping observation → separately confirmed minimum-speed start/stop → bounded paired targets → pause if required → conservative full example. Communication interruption is manual, supervised and only after reliable physical stop access and simulation results. Capture link/status behavior, measured response envelopes and failures, not just app exit code.

## Release acceptance

A v0.1 hardware release requires repeatable validated W1 evidence for every enabled control and observation, green fault-injection/simulation tests, no auto-start/restart path, a second workout without code edits, bounded stop with truthful unconfirmed outcomes, clean documented Linux build and license/runtime inventory. Unsupported pause or incline is explicitly disabled. If stop/motion cannot be reliably observed, diagnostics/simulation can be delivered but automatic W1 workout acceptance remains unmet. Test results must distinguish simulation, fake transport and physical runs.
