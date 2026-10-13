# Internal controller contract

This is an internal C++17/Qt API, not a stable SDK or approved W1 controller.
The CLI refuses hardware workouts. Device identity, calibration, initialization
and physical feedback still require the postponed hardware validation.

## Ownership and threading

Create the controller, queue, runner and transport on one Qt event-loop thread.
Call their methods and deliver raw frames on that same thread. Signals may call
slots synchronously and reenter the queue; callers must allow that. Cross-thread
calls, moving an active controller to another thread, and blocking the event loop
are unsupported. No worker-thread API or queued metatype contract is provided.

`Runner` borrows its `Controller`; destroy the runner first. `FitShowController`
borrows `FitShow::Control`, its clock and lifecycle hooks: their captured data
must outlive the adapter. `QzController` owns its transport and keeps it alive
through the base-class hooks. QObject parenting is optional and does not change
the lifetime requirements of borrowed references. Do not give stack objects a
parent that could delete them before their scope ends.

Use a fresh controller/runner pair for each workout session. `prepare()` is a
one-time operation; a finished runner cannot be reset. The queue invalidates
generations on cancellation/disconnection; creating a new runner with its
initial generation against a previously used queue is unsupported. There is no
automatic session restart or resume after connection loss.

Destruction/disconnection closes resources, but does not verify stopping. Before
normal shutdown, request `abort()`, keep the event loop and advancement running,
and wait for `finished()`. Report `StopUnconfirmed` explicitly. QzController's
destructor closes BLE; it is not an emergency-stop mechanism.

## Time and advancement

Call `Runner::advance(nowMs)` regularly using monotonic elapsed milliseconds
from one clock origin. `prepare(nowMs)` uses that same origin. Advancement calls
the controller; do not separately advance the controller in a runner session.
The current loop-gap limit is 1.5 seconds; normal CLI ticks are 100 milliseconds.
All timing defaults are provisional software policy, not validated W1 deadlines.

The QZ transport independently pumps its queue using its elapsed clock.
`FitShowController` receives a monotonic clock function to timestamp new raw
motor reports. Its origin may differ from the runner's: `advance()` subtracts
the report's age from the runner's time. Clock reversal and reports older than
two seconds fault the session. ACKs, connection events and cached values do not
renew motor freshness. The adapter watchdog requires regular `advance()` calls;
it cannot act while the event loop/process is stalled.

Preflight allows up to ten seconds for the first usable report after connection,
identity and capability discovery. It requires three stopped reports before
Ready. No motor commands are authorized by an absent report. After the first
report, normal freshness checks also apply during preflight.

## Values and asynchronous outcomes

`Target` uses km/h and percent grade. A target is always a consistent pair.
Profiles convert to verified raw speed units and discrete incline codes; unknown
geometry/evidence disables automatic execution. Offline mapping preview can use
unverified geometry and never grants motor authorization.

`Telemetry::atMs` is the report time in the caller's clock domain, not the ACK
time. Raw speed is retained even in stopped states. `running`, `paused`, speed
and grade are observations; successful submission/acknowledgement does not prove
physical motion or stopping.

Request methods return immediately. `commandEvent(id, phase)` reports
`submitted`, `acknowledged`, `rejected`, `canceled` or `timed_out`; separate
runner events report settling and acknowledgement deadlines. IDs identify
logical commands and must increase within a session. FitShow start and its
initial target share one ID and can each emit submission/ACK events. ID zero is
reserved for internal diagnostics. FitShow ACKs match command/subtype only and
cannot uniquely associate a delayed reply with a particular target pair.
No timed-out motor request is automatically retried.

Generation tokens invalidate queued automation. Stop changes generation,
purges queued targets and overtakes an outstanding command. It cannot retract
bytes already delivered to the peripheral or guarantee its behavior. Faults
latch the adapter; later good packets cannot resume automation. Faulted or
missing feedback cannot certify stopping, even if a fake device stops in a test.

## Intended library boundary

| Component | Dependency / ownership | Future role |
|---|---|---|
| Targets, capabilities, telemetry, Controller | Qt Core | Candidate public data and async interface |
| Workout model, loader, Runner, Simulator | Qt Core; Runner borrows Controller | Candidate reusable engine |
| DeviceProfile, FitShowController | Qt Core; adapter borrows restricted queue | Internal calibration and protocol binding |
| FitShow::Control and packet helper | Qt Core; explicit transport permission | Internal queue/decoder; shared with QZ |
| QzController and restricted QZ transport | Qt Bluetooth/Positioning; owned transport | Internal Linux backend |
| Diagnostics, session, terminal, probe | CLI policy/resources | Keep outside the reusable library |

`driver/driver.pro` is a bounded link target, not an installed SDK. Preserve
the repository GPL terms and corresponding source when distributing it.
Public API/versioning and library extraction follow hardware validation;
do not add a general SDK layer solely for the current CLI.
