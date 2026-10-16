# Project Planning: Treadmill — Linux CLI Treadmill Controller

## Your Role

You are a senior C++/Qt software engineer, Bluetooth Low Energy (BLE) protocol specialist, and software architect.

You are working inside a fork of the open-source **QDomyos-Zwift** project:

https://github.com/cagnulein/qdomyos-zwift

Your task is to investigate the existing codebase and develop a complete, technically grounded implementation plan for a new project called **treadmill**.

The project will initially be a proof-of-concept Linux command-line application controlling a **RunPad W1 treadmill** over Bluetooth, using the **FitShow BLE protocol**.

The application will execute predefined training plans, automatically adjusting treadmill speed and incline while providing real-time status and advance notifications.

The first proof of concept should demonstrate a **20-minute training session aimed at hiking and ski-touring preparation**, including warm-up, uphill intervals, recovery, and cooldown.

This workout is only an initial example. The software must not be designed specifically around hiking, ski touring, or a single training plan.

The longer-term objective is to extract the core functionality into a reusable library.

**Your immediate task is PLANNING, not implementation.**

Do not begin modifying production code or implementing features. First investigate the repository, verify assumptions, identify technical risks, and produce an actionable implementation plan.

Do not treat technical assumptions in this prompt as verified facts. Inspect the actual source code and distinguish confirmed behavior from assumptions requiring hardware testing.

---

# 1. Project Objectives

## Primary objective

Build a Linux CLI application named `treadmill` that:

1. Connects to the RunPad W1 treadmill using Bluetooth Low Energy.
2. Communicates using the FitShow protocol implemented by QDomyos-Zwift.
3. Discovers and validates treadmill capabilities.
4. Controls treadmill start, stop, speed, and incline.
5. Executes configurable, predefined workout plans.
6. Supports warm-up, intervals, recovery, cooldown, and speed/incline ramps.
7. Displays current workout progress and upcoming transitions.
8. Provides advance warnings before speed or incline changes.
9. Reads actual treadmill telemetry and verifies requested changes.
10. Allows the user to interrupt or terminate a workout.
11. Handles Bluetooth errors, unexpected treadmill states, and application interruptions.
12. Records diagnostic information and workout results.
13. Runs headlessly on Linux without requiring a graphical interface.
14. Has an architecture suitable for eventual extraction into a reusable library.

## Design principles

- The CLI is named `treadmill`.
- The target hardware for the proof of concept is RunPad W1.
- Workout definitions must be independent of the hardware implementation.
- The application must not hard-code a single workout.
- The architecture should allow different workout types in the future.
- Prefer existing QZ functionality over reimplementing it.
- Keep the initial implementation small and focused.
- Prioritize predictable physical-device control and safety.

## Non-goals for the initial proof of concept

Do not prioritize:

- Graphical user interfaces.
- Zwift integration.
- Strava or Garmin integration.
- Cloud synchronization.
- Heart-rate-based automatic speed control.
- Multi-user accounts.
- Support for unrelated treadmill manufacturers.
- Mobile applications.
- Calorie estimation.
- Advanced training analytics.

---

# 2. Repository Investigation

Before proposing an architecture, inspect the actual QDomyos-Zwift source code.

Understand:

- Overall repository structure.
- Build system and Linux support.
- C++ and Qt versions.
- Bluetooth dependencies.
- Device discovery and selection.
- FitShow protocol implementation.
- Existing treadmill control interfaces.
- Existing workout scheduling infrastructure.
- Existing headless execution capabilities.
- Existing logging and configuration systems.
- Existing tests and test infrastructure.
- Existing command queues, retry mechanisms, and event loops.
- Licensing requirements.

Pay particular attention to these potential locations:

- `src/devices/fitshowtreadmill/fitshowtreadmill.cpp`
- `src/devices/fitshowtreadmill/fitshowtreadmill.h`
- `src/trainprogram.cpp`
- `src/trainprogram.h`

These are investigation starting points, not guaranteed interfaces. Confirm they exist in the checked-out revision.

Investigate the documented `-no-gui` and `-train` functionality and determine whether it can be reused.

## Questions to answer

1. How does QZ discover and connect to FitShow treadmills?
2. Which BLE services and characteristics does it support?
3. How are FitShow packets constructed and parsed?
4. How are speed and incline represented and converted?
5. Are speed and incline transmitted together?
6. How are start, stop, and pause commands implemented?
7. How are acknowledgements, notifications, retries, and timeouts handled?
8. How does QZ detect that the treadmill is running?
9. Can the application distinguish a submitted command from a physically completed operation?
10. How are workout programs represented and scheduled?
11. Does the existing workout engine support ramps, notifications, and repeated intervals?
12. Can existing workout functionality run headlessly on Linux?
13. Can the FitShow driver be reused without starting unrelated QZ subsystems?
14. Which dependencies would be required for a minimal CLI executable?
15. What modifications would make future upstream merges easier?

Reference actual source files, functions, and line numbers where practical.

Do not invent APIs or assume functionality exists merely because its name suggests it does.

---

# 3. Hardware Target: RunPad W1

The initial hardware target is the RunPad W1 treadmill, which is advertised as supporting the FitShow application.

Reported specifications include:

- Bluetooth connectivity.
- Motorized incline.
- Eight incline settings.
- Speed adjustment.
- A maximum advertised incline of approximately 11%.

These specifications must be verified against the exact hardware variant.

We do not yet know whether the W1 uses precisely the same FitShow BLE behavior as devices already supported by QZ.

## Required hardware discovery

Plan a diagnostic workflow to determine:

- BLE device name and stable identification.
- Advertised services.
- GATT service UUIDs.
- Write characteristics.
- Notification characteristics.
- Device/model/firmware information, if exposed.
- Supported speed range and resolution.
- Supported incline range and resolution.
- Speed units and protocol scaling.
- Mapping between FitShow incline values and physical incline settings.
- Status notification format.
- Command acknowledgement behavior.
- Whether actual speed and incline can be reliably read back.
- Whether initialization, pairing, authentication, or a specific command sequence is required.
- Whether multiple simultaneous controllers are supported.

Investigate whether QZ supports multiple FitShow GATT characteristic mappings and how they are selected.

## Hardware testing constraints

All initial discovery must be read-only.

Motor commands must only be introduced after capability validation and explicit user confirmation.

Do not assume that successful BLE connection means motor-control compatibility.

Create a hardware validation matrix identifying what is confirmed from source code, what requires physical testing, and what remains unknown.

---

# 4. Proposed Application Architecture

The target conceptual architecture is:

```text
treadmill (CLI)
    |
    v
CLI / Terminal Interaction
    |
    v
Workout Engine
    |
    v
Treadmill Controller Interface
    |
    v
QZ FitShow Driver
    |
    v
Qt Bluetooth / Linux BlueZ
    |
    v
RunPad W1
```

Treat this as a proposal, not a mandatory implementation.

Evaluate whether the existing QZ architecture already provides suitable abstractions.

Prefer reusing existing components over duplicating them.

## Architecture requirements

Separate the following concerns.

### CLI layer

- Terminal interaction.
- Progress reporting.
- User commands.
- Notifications.
- Error presentation.

### Workout engine

- Workout definition.
- Workout loading and validation.
- Stage timing.
- Transition scheduling.
- Pause/abort behavior.
- Upcoming-stage notifications.
- Workout completion.
- Support for multiple workout plans.

### Treadmill controller

- Connection lifecycle.
- Device capability validation.
- Start/stop operations.
- Speed and incline targets.
- Telemetry.
- Safety constraints.
- Device state.

### FitShow protocol

- Packet encoding.
- Packet decoding.
- GATT communication.
- Notifications.
- Acknowledgements.
- Protocol-specific state.

### Persistence

- Configuration.
- Session logs.
- Workout results.
- Diagnostic traces.

The treadmill controller should eventually be usable independently of the CLI.

## Architectural alternatives

Evaluate at least:

**Option A:** Extend QZ's existing headless application and workout engine.

**Option B:** Create a small standalone CLI executable reusing QZ's FitShow driver and selected supporting classes.

**Option C:** Refactor necessary QZ components into a reusable internal library and build the CLI on top.

Compare:

- Implementation complexity.
- Code reuse.
- Dependency footprint.
- Maintainability.
- Testing difficulty.
- Compatibility with upstream QZ.
- Suitability for future library extraction.
- Risk of changing existing QZ behavior.

Recommend one approach with justification.

Prefer C++/Qt for the initial implementation unless repository investigation provides a compelling reason otherwise.

Do not introduce Python/Bleak or rewrite the FitShow protocol merely for convenience.

---

# 5. Workout System Requirements

The application must support **configurable workout plans**.

A workout should consist of ordered stages that specify:

- Duration.
- Target speed or speed range.
- Target incline or incline range.
- Optional speed/incline ramps.
- Stage name.
- Optional stage description.
- Optional user-facing instructions.
- Optional transition notifications.

The workout engine must not contain hard-coded assumptions about hiking or ski touring.

## Initial proof-of-concept workout

Create one example workout lasting approximately 20 minutes.

Its training goal is to support hiking and ski-touring preparation through controlled uphill walking.

It should include:

- Warm-up.
- Progressive uphill walking.
- Sustained climbing intervals.
- Active recovery.
- A second climbing effort.
- Cooldown.
- Verified treadmill stop.

The following is an **illustrative example, not a mandatory final workout definition**:

| Time | Stage | Target speed | Incline |
|---|---|---|---|
| 00:00–03:00 | Warm-up | 2.0–2.6 km/h | Flat |
| 03:00–06:00 | Gentle ascent | 3.0 km/h | Low |
| 06:00–09:00 | Steady climb | 3.3 km/h | Moderate |
| 09:00–11:00 | Active recovery | 2.5 km/h | Low |
| 11:00–14:00 | Second climb | 3.6 km/h | Moderate |
| 14:00–16:00 | Easier walking | 2.8 km/h | Low |
| 16:00–20:00 | Cooldown | 2.4 → 1.5 km/h | Flat |

These are preliminary, adjustable targets, not validated prescriptions for a particular user.

The planning agent should evaluate this example and propose improvements where appropriate.

Do not hard-code this workout's name, stages, durations, speeds, or incline settings into the application.

## Workout design requirements

1. Workout definitions must be hardware-independent.
2. Actual incline values must be mapped to verified device capabilities.
3. Speed units must be explicit.
4. Workout targets must not exceed verified hardware limits.
5. Changes should be gradual and predictable.
6. Timing should use a monotonic clock.
7. The workout should not begin until the treadmill has confirmed a suitable running state.
8. Transitions must account for hardware response delays.
9. User-requested reductions in intensity must take priority over the plan.
10. Completion must include controlled cooldown and verified stop.
11. Workout plans should be loadable from external files or an existing QZ-supported format.
12. The engine should allow adding new workouts without changing application code.

Investigate whether QZ's existing XML workout format can represent these requirements.

If it can, recommend reusing it.

If it cannot, explain the limitations and propose the smallest necessary extension.

Do not introduce a new workout format without justification.

## Training considerations

Assess whether the proposed example is reasonable as a beginner-friendly prototype.

Explain the limitations of treadmill training for hiking and ski touring, including the lack of downhill eccentric loading, uneven terrain, and carrying equipment.

Consider perceived exertion as optional user guidance.

Do not introduce heart-rate-based automatic control in version 0.1.

---

# 6. CLI User Experience

The executable should be named `treadmill`.

Proposed commands:

```bash
treadmill scan
treadmill info
treadmill status

treadmill workout list
treadmill workout run <workout-id>
treadmill workout run <workout-id> --simulate
```

For example:

```bash
treadmill workout list
treadmill workout run hiking-20
treadmill workout run hiking-20 --simulate
```

The workout identifier `hiking-20` is illustrative. It is not a mandatory name.

Evaluate the most appropriate CLI syntax and command structure.

## Startup workflow

1. Discover or select the treadmill.
2. Connect over BLE.
3. Read capabilities.
4. Verify speed and incline configuration.
5. Confirm a safe initial treadmill state.
6. Display workout summary.
7. Request explicit confirmation.
8. Start the treadmill.
9. Confirm actual movement before starting the workout timer.

## During the workout

Display:

- Elapsed time.
- Remaining time.
- Current stage.
- Current measured speed.
- Current measured incline.
- Requested speed and incline when different from measured values.
- Next stage.
- Time until the next transition.
- BLE connection and telemetry status.
- Warnings and errors.

Example:

```text
TREADMILL
Connected: RunPad W1

Workout: Hiking Preparation
Progress: 07:42 / 20:00

Current stage: STEADY CLIMB
Actual speed: 3.3 km/h
Incline: Moderate

NEXT IN 00:18
Active Recovery
Target speed: 2.5 km/h
Target incline: Low

[P] Pause  [R] Resume  [S] Stop  [Q] Quit
```

This is illustrative output, not an existing implementation.

## Transition notifications

Plan notifications at:

- 30 seconds before transition.
- 10 seconds before transition.
- The moment of transition.

Notifications should describe the upcoming change.

Investigate QZ's existing text-event support.

Consider optional terminal bell or spoken notifications, but keep them outside the minimum proof-of-concept scope unless trivial.

## Manual controls

Plan support for:

- Stop/abort.
- Quit.
- Pause/resume, only if hardware behavior is verified.
- Optional reduction in workout intensity.
- Optional skip to the next stage.

Terminal controls must not interfere with the application's event loop or BLE communication.

Consider what happens when terminal input is unavailable or standard input closes.

---

# 7. Safety Requirements

**Safety is a primary architectural requirement.**

This application controls physical exercise equipment capable of causing injury.

A successful Bluetooth write does not guarantee that the treadmill has physically changed state.

## Mandatory principles

### Capability validation

Never issue automatic motor commands using unverified speed or incline limits.

If capability discovery fails, automatic workout execution must be disabled.

### Explicit user confirmation

Require confirmation before starting the belt.

### Initial state validation

Do not automatically start if the treadmill is already moving or its state cannot be determined.

### Combined speed/incline commands

Investigate whether QZ sends speed and incline together.

If so, every workout transition must use a consistent target pair and avoid unintentionally restoring stale values.

### Command lifecycle

Distinguish:

1. Command created.
2. Command submitted.
3. Command acknowledged.
4. Target observed in telemetry.
5. Target reached, rejected, or timed out.

### Stop priority

Stop requests must take precedence over queued workout commands.

After abort, no pending workout command may restart or accelerate the treadmill.

### Telemetry freshness

Detect stale or missing telemetry even when BLE remains connected.

### Connection loss

On connection loss:

- Suspend workout automation immediately.
- Cancel pending transitions.
- Alert the user.
- Never automatically resume motor control after reconnection.
- Require explicit confirmation before subsequent workout execution.

Do not claim software can stop the treadmill when Bluetooth communication is unavailable.

### Physical controls

The treadmill's physical emergency-stop mechanism must remain the primary safety mechanism.

Software stop is not a substitute for physical emergency stopping.

### Manual intervention

If the user changes speed, incline, or running state through physical controls, the application must not blindly override those changes.

Propose a safe manual-override policy.

### Application shutdown

Handle normal exit, Ctrl+C, and recoverable termination signals.

Attempt stop when communication is available and verify the result.

Recognize that process crashes, SIGKILL, operating-system failures, or Bluetooth failure may prevent software stop.

### Stop verification

Distinguish:

- Stop requested.
- Stop acknowledged.
- Belt confirmed stopped.
- Stop unconfirmed.

If stopping cannot be confirmed, display a prominent warning to use the physical stop mechanism.

### No unexpected restart

Never automatically restart after:

- Bluetooth reconnection.
- Application restart.
- Physical emergency stop.
- User abort.
- Device reset.

## Required safety deliverables

Produce:

- Hazard and failure-mode analysis.
- Proposed mitigations.
- Stop/abort policy.
- Reconnection policy.
- Manual-override policy.
- Command timeout and retry recommendations.
- List of behaviors that cannot be guaranteed in software.

Do not claim that this proof of concept is safety-certified.

---

# 8. Workout State Machine

Design an explicit state machine.

Suggested states:

```text
DISCONNECTED
    |
CONNECTED
    |
PREFLIGHT
    |
READY
    |
STARTING
    |
RUNNING
    |
STOPPING
    |
COMPLETED
```

Additional states:

```text
PAUSED
ABORTED
FAULT
STOP_UNCONFIRMED
```

These are proposals, not necessarily the final design.

For every state, specify:

- Entry conditions.
- Allowed commands.
- Required telemetry.
- Timeout behavior.
- Exit conditions.
- Error transitions.
- Whether motor commands are permitted.

Pay particular attention to races between:

- Workout stage transitions.
- BLE acknowledgements.
- Telemetry updates.
- Manual stop requests.
- Reconnection events.
- User input.
- Application shutdown.

Explain how stale queued commands are prevented from executing after an abort.

---

# 9. Configuration and Session Logging

Plan configuration for:

- Treadmill identifier.
- Speed units.
- Verified capability limits.
- Incline mapping.
- Notification preferences.
- Workout selection.
- Workout file locations.
- Optional safety limits.
- Logging configuration.

Device-specific calibration must be separated from generic FitShow protocol behavior.

## Session logging

Record:

- Session start/end timestamps.
- Workout identifier.
- Workout stage transitions.
- Requested speed and incline.
- Actual reported speed and incline.
- BLE connection events.
- Command submissions.
- Acknowledgements.
- Timeouts.
- Errors.
- User actions.
- Workout completion or abort reason.

Recommend an initial storage format, such as JSON Lines.

Avoid recording unnecessary personal information.

Provide detailed protocol diagnostics without flooding normal terminal output.

---

# 10. Simulation and Testing

A simulator is required so most workout development can happen without operating the physical treadmill.

The simulator should implement the same controller interface as real hardware.

Support:

- Connection and disconnection.
- Capability reporting.
- Start and stop.
- Speed and incline changes.
- Simulated movement and incline delays.
- Telemetry updates.
- Command acknowledgements.
- Command rejection.
- Communication timeouts.
- Stale telemetry.
- Unexpected device state.
- Manual overrides.
- Stop failures.

## Test categories

### Protocol tests

- Frame encoding/decoding.
- Checksums.
- Valid and invalid packets.
- Unit conversion.
- Capability parsing.
- Notification parsing.

### Workout tests

- Stage order.
- Configurable workout duration.
- Warm-up and cooldown.
- Speed/incline target generation.
- Upcoming-stage notifications.
- Pause/abort timing.
- Monotonic-clock behavior.
- Loading and validating different workout plans.

### Safety tests

- Abort during speed change.
- Abort during incline change.
- Stop while commands are queued.
- Bluetooth disconnect during workout.
- Stale telemetry.
- Failed acknowledgements.
- Physical manual override.
- Application shutdown.
- Reconnection without automatic restart.

### Integration tests

- CLI + workout engine + simulator.
- Controller + FitShow driver.
- Existing QZ workout engine integration, if reused.

### Hardware tests

- Read-only discovery.
- Speed validation.
- Incline validation.
- Start/stop verification.
- Full workout at conservative settings.
- Recovery from interrupted communication.

Hardware tests must be explicitly separated from tests safe to run in CI.

Never execute motor-control integration tests automatically against real hardware.

---

# 11. Development Milestones

Produce a phased roadmap with dependencies, deliverables, acceptance criteria, and risks.

## Milestone 0 — Repository and Architecture Assessment

Deliverables:

- Repository analysis.
- FitShow driver analysis.
- Existing workout engine analysis.
- Linux build analysis.
- Architecture recommendation.
- Risk register.
- Hardware validation plan.

No production implementation.

## Milestone 1 — Hardware Compatibility

Deliverables:

- Linux build environment.
- Read-only BLE diagnostics.
- Device identification.
- Capability discovery.
- Speed unit verification.
- Incline mapping investigation.
- Controlled start/stop testing.
- Telemetry verification.

Acceptance criterion:

Required W1 controls and observable states are validated, or unsupported operations are explicitly identified.

## Milestone 2 — Minimal Treadmill Controller

Deliverables:

- Clean controller interface.
- Connection lifecycle.
- Speed/incline control.
- State tracking.
- Stop priority.
- Timeouts.
- Error handling.
- Simulator.

Acceptance criterion:

Basic operations are reliable and testable independently of the workout CLI.

## Milestone 3 — Workout Engine

Deliverables:

- Configurable workout loading.
- Initial 20-minute example workout.
- Stage transitions.
- Speed/incline ramps.
- Recovery and cooldown.
- State machine.
- Transition notifications.
- Automated simulation tests.

Acceptance criterion:

The example workout executes correctly in simulation, and another workout can be added without modifying application code.

Reuse QZ's existing workout scheduler if appropriate.

## Milestone 4 — CLI Integration

Deliverables:

- `treadmill` executable.
- Workout selection.
- Interactive controls.
- Live telemetry.
- Progress display.
- Upcoming-stage messages.
- Session logs.
- Clean shutdown handling.

Acceptance criterion:

The CLI runs a complete workout through the simulator and, after hardware validation, on the W1.

## Milestone 5 — Hardening and Packaging

Deliverables:

- Failure-injection tests.
- Hardware test documentation.
- Linux build instructions.
- CI configuration.
- Reproducible builds.
- Documentation.
- Stable controller API.
- Library extraction plan.

Acceptance criterion:

The proof of concept is reproducible, testable, and structured for future reuse.

---

# 12. Future Library Design

The longer-term goal is to extract the controller into a reusable library.

Design for this from the beginning, but do not prematurely implement a general-purpose SDK.

Consider an eventual interface conceptually similar to:

```cpp
class TreadmillController {
public:
    virtual void connect() = 0;
    virtual void disconnect() = 0;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual void setTarget(
        double speedKmh,
        double incline
    ) = 0;

    virtual TreadmillState state() const = 0;
    virtual TreadmillCapabilities capabilities() const = 0;
};
```

This is only a conceptual example.

Adapt the API to Qt's asynchronous programming model, signals/slots, error reporting, and actual QZ architecture.

Do not design a synchronous API that falsely implies BLE commands complete immediately.

Explain which abstractions should eventually become public and which remain internal.

---

# 13. Build, Licensing, and Upstream Maintenance

Investigate:

- Linux distribution requirements.
- Qt build dependencies.
- Bluetooth/BlueZ requirements.
- Permissions and D-Bus requirements.
- Build system complexity.
- Headless operation.
- Packaging strategy.
- CI feasibility.
- Compatibility with upstream QZ changes.

Document the exact repository revision used for planning.

Prefer minimizing invasive modifications to upstream code.

Consider whether new functionality should live in a separate directory or target.

Check the repository's actual license files and relevant third-party licensing obligations.

Do not assume extracting code into a library permits changing its license.

---

# 14. Required Planning Deliverables

Create the following Markdown documents in the repository, preferably under `docs/treadmill/`.

### `01-repository-analysis.md`

Include:

- Existing architecture.
- Relevant source files.
- FitShow protocol implementation.
- Workout engine capabilities.
- Headless Linux capabilities.
- Reusable components.
- Limitations and technical debt.
- Source-code references.

### `02-architecture.md`

Include:

- Recommended implementation approach.
- Alternatives considered.
- Component boundaries.
- Dependency diagram.
- Data flow.
- Public and internal interfaces.
- Threading/event-loop considerations.
- Future library extraction strategy.

### `03-hardware-validation.md`

Include:

- RunPad W1 compatibility assumptions.
- BLE discovery plan.
- Protocol validation checklist.
- Speed/incline calibration procedure.
- Hardware test matrix.
- Safe testing prerequisites.
- Unknowns requiring physical-device access.

### `04-workout-system.md`

Include:

- Workout definition model.
- Workout file format recommendation.
- Example 20-minute workout.
- Training objectives.
- Stage durations.
- Speed/incline transitions.
- Notification schedule.
- Warm-up/recovery/cooldown.
- Hardware-independent incline representation.
- Workout timing semantics.
- Adaptation to verified device capabilities.
- Strategy for supporting additional workouts.

### `05-safety-and-state-machine.md`

Include:

- State diagram.
- State transition table.
- Command lifecycle.
- Stop/abort behavior.
- Reconnection behavior.
- Manual overrides.
- Hazard analysis.
- Failure-mode handling.
- Limits of software-based safety.

### `06-testing-strategy.md`

Include:

- Unit tests.
- Protocol tests.
- Simulator architecture.
- Integration tests.
- Hardware tests.
- Fault injection.
- CI strategy.
- Acceptance criteria.

### `07-implementation-roadmap.md`

Include:

- Milestones.
- Ordered implementation tasks.
- Dependencies.
- Estimated complexity.
- Risks.
- Deliverables.
- Definition of done for each milestone.

Tasks should be small enough for a coding agent to implement and validate incrementally.

### `08-open-questions.md`

Include:

- Unverified assumptions.
- Hardware-dependent questions.
- Architectural decisions awaiting evidence.
- Potential blockers.
- Questions requiring user input.

Prioritize open questions by impact.

---

# 15. Final Response Format

After completing repository investigation and writing the planning documents, provide a concise summary containing:

1. **Repository findings** — what QZ already implements and what is missing.
2. **Recommended architecture** — which approach to use and why.
3. **Major technical risks** — especially FitShow compatibility and safe motor control.
4. **Implementation roadmap** — recommended milestone order.
5. **First coding task** — smallest meaningful implementation step.
6. **Hardware questions** — what must be tested on the physical W1.
7. **Files created** — paths to planning documents.

Clearly distinguish:

- Confirmed from source code.
- Inferred from source code.
- Unverified hardware assumptions.
- Design recommendations.

If behavior cannot be verified, state that explicitly rather than guessing.

---

# 16. Operating Instructions

Follow these rules:

1. **Inspect before designing.** Read relevant QZ implementation before recommending changes.
2. **Reuse before rewriting.** Prefer existing QZ components when suitable.
3. **Plan before coding.** Do not modify production source files in this task.
4. **Prioritize physical safety.** Never assume Bluetooth communication guarantees safe motor control.
5. **Design for testability.** Separate simulated and real treadmill implementations.
6. **Keep scope focused.** This is a Linux CLI proof of concept.
7. **Keep workouts generic.** Do not hard-code a particular training discipline or workout.
8. **Protect upstream compatibility.** Minimize unnecessary changes to QZ internals.
9. **Avoid unnecessary dependencies.** Prefer existing Qt/C++ infrastructure.
10. **Document uncertainties.** Do not hide protocol assumptions or missing hardware information.
11. **Produce actionable tasks.** The implementation plan must be executable by another coding agent.
12. **Preserve the current application.** Planning must not break existing QZ functionality.
13. **Do not claim untested hardware support.**
14. **Do not run physical motor-control commands** during repository investigation.
15. **Do not create implementation commits** during planning unless explicitly instructed.

## Definition of Done

Your planning work is complete when:

- You have inspected relevant QDomyos-Zwift source code.
- You have identified existing FitShow and workout capabilities.
- You have recommended a concrete architecture.
- You have documented hardware validation.
- You have designed a generic workout system.
- You have specified one example 20-minute workout.
- You have defined safety and failure-handling requirements.
- You have proposed a testable state machine.
- You have designed the simulator/testing approach.
- You have created the eight planning documents.
- You have produced an ordered implementation roadmap.
- You have identified the first implementation task.
- You have clearly listed remaining uncertainties.

**Begin by inspecting the repository and understanding its FitShow driver, workout scheduler, and Linux headless execution paths. Do not start coding until the architecture and implementation plan have been documented.**
