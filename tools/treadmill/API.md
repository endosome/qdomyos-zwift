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

## Manual control (for a TUI wrapper)

`treadmill workout run <id> --backend linux --manual-control [--manual-strategy offset|hold|takeover] ...` lets an operator
interface raise or lower speed and incline during a hardware workout. Linux backend only.

**Transport.** The CLI is the child process; the wrapper talks to it over stdin and stdout.
- stdout: one compact JSON object per line, flushed per line, the same records the CLI writes to its `--log` file.
  The wrapper **must keep reading stdout**: if the CLI cannot write, the helper stops the belt (fail-safe).
- stdin: one command per line. Because both the stdout stream and the log file carry every record, a command and its
  outcome can be compared on either side when diagnosing the link.
- The CLI still insists on a terminal on stdin and on the `MOTOR` confirmation line, so the wrapper has to give the child a
  pty and pass the operator's confirmation through. This was deliberately not relaxed. The pty is set to raw, no-echo mode.
- Before the run: send `MOTOR`, then an empty line (ENTER) to start after `--wait-wake` (or `q` to cancel).
- Closing stdin stops the belt ("Operator input closed").

**Commands** (one JSON object per line): `{"seq": 7, "cmd": "speed_up"}` with `cmd` one of `speed_up`, `speed_down` (0.1 km/h),
`incline_up`, `incline_down` (one W1 level), `reset` (back to the plan's target for the current step), `stop`. A bare `q` line also stops.
`seq` is echoed back; use a new number for each command.

**Records.** `manual_command_received {seq, cmd}` is written for every line (with `raw` when it was not valid JSON).
Then `manual_command_result {seq, cmd, result: "applied"|"refused", reason?, speed_code, incline_code}`; the codes are the
target now in force (speed in 0.1 km/h, incline level 0-8). A refused command changes nothing: a step beyond the limits
(speed 1.0 up to 6.0 km/h, or the validated running speed with `--running-mode`; incline 0-8) is refused, not clamped.
An applied command is followed by the usual `write_requested`, `command`, `raw_status` and `reported_target` records.

**Strategies** (`--manual-strategy`, default `takeover`):
- `takeover`: after the first adjustment the plan no longer sets targets. Its timeline still runs, so the workout ends at its scheduled time.
- `offset`: each adjustment shifts the current and all later plan steps (limits apply per step).
- `hold`: an adjustment lasts until the plan's next step, which then applies as written.

In every strategy the plan's step timing is not restarted by an adjustment.

While the console is paused or the run is returning to the plan (see below), commands are refused with the reason
"the console is paused or returning to the plan".

**Safety.** The helper is started with `--manual-max-speed <code>`. With it, the helper's gate approves any target with
speed 10..limit and incline 0..8 (at most 2000 writes) instead of only the plan's sequence, and the same limits become the
feedback ceilings. Start, stop, stale-status, deadline and bounds checks are unchanged.

## Console Pause/Play records

On the Linux backend, a workout continues across a pause from the console (README, "Pause and Play on the console").
Once the belt has run, STOPPING (4), NORMAL (0) or countdown (2) without a host STOP counts as a pause. The records are:

- `console_pause {status_code, stage, resume_speed_code, resume_incline_code, paused_seconds_so_far}`: the run is paused.
  The plan clock is frozen and nothing is sent until the treadmill reports RUNNING again.
- `console_play {speed_code, incline_code, stage}`: the console restarted the belt (first RUNNING report after the pause).
- `resume_target_requested {stage, speed_code, incline_code, plan_speed_code, plan_incline_code}`: one step of the return,
  at most +5 speed codes and +1 incline level above the previous step. A value above the plan's goes straight to the plan's.
- `console_resumed {stage, speed_code, incline_code, pause_and_ramp_seconds, paused_seconds_total}`: back at the plan's
  target. The step's hold continues from where it was paused.
- `trial_result` adds `console_pauses` and `console_paused_seconds`.

A pause during the return starts a new `console_pause` and the return starts again after Play.

**Safety.** The helper is started with `--pause-allowance 900`. After an observed pause, its gate extends the motor deadline
by the allowance and stops the run when the paused (non-RUNNING) time exceeds it. It approves targets only while the console
reports RUNNING, and only inside the plan's own speed and incline ceilings (at most 2000). Sleep, key and fault states still stop the run.

