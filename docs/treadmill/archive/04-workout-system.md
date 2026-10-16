# Generic workout system

This is a **design recommendation**. Existing behavior and limitations are documented in [01](01-repository-analysis.md); no executable workout or production parser is created in this task.

## Reuse the QZ XML dialect

Retain `<rows>`, `<row duration="hh:mm:ss" speed="..." inclination="..." forcespeed="1">`, `<repeat times="N">`, speed ramp attributes `speedfrom/speedto`, and `<textevent timeoffset="seconds" message="..."/>`. Source: `src/trainprogram.cpp:1961`, `:1974`, `:2108`, `:2278`. Do not introduce JSON/YAML workout definitions. Generic config/device profiles and JSON Lines logs are separate concerns.

The current format covers ordered intervals, repeats, fixed speed/incline, speed ramps and text events. It does not fully meet stage metadata, explicit units, incline ramps, monotonic scheduling or physical target verification. Reusing its scheduler unchanged would retain wall-clock timing and enqueue-time start behavior. Recommend a focused runner over generic stages, retaining and extracting the applicable XML parsing logic rather than copying the whole integration-heavy scheduler. Legacy QZ load/save remains unchanged until a separately reviewed shared-parser integration.

Small additive CLI dialect extensions (proposed, not supported by current QZ): root `version="1"`, `id`, `name`, `speedunit="km/h"`, `inclineunit="percent"`; row `name`, `description`, `instruction`, and paired `inclinationfrom/inclinationto`. Root version identifies CLI validation rules. `speedfrom/speedto` retain linear ramp semantics; constant `inclination` retains percent interpretation in the CLI. Current QZ may ignore extensions and would not implement incline ramps; mark such files as requiring the CLI, not interchangeable without qualification. CLI validator rejects unknown motor-affecting attributes, malformed XML, duplicate IDs, nested repeats and unsupported power/HR/GPX controls rather than ignoring them.

Generic stage: name, optional description/instruction, positive duration, speed in explicit km/h (constant or linear endpoints), incline in percent (constant or linear endpoints), and row-relative text events. A selectable range can use existing `minspeed/maxspeed` as a user choice only when `speed` explicitly chooses a point within it; no HR automatic adjustment. Incline range choices can wait until needed: v0.1 supports constant/ramp endpoints and user-requested reductions. Do not invent an automatic range-selection policy.

Validate finite values, duration between one second and an explicit plan limit below 24 hours for the QTime dialect, total bounded duration, positive repeat counts, bounded expanded stage count, coherent range/ramp attributes, no ambiguous simultaneous constant and ramp values, and events within the row. Expand repeats into logical stages with distinct occurrence IDs. Keep logical ramps intact; do not inherit QZ's expansion off-by-one endpoint behavior or lose child text events. Use checked Qt numeric conversions and explicit XML errors, no regular expressions.

## Example: 20 minutes of uphill walking

The following is an editable prototype, not a validated prescription for a particular person or W1. Proposed numerical inclines are generic percent grade and require calibrated W1 mapping. Use lower speeds/grades when needed; reject the example on incompatible minimum speed or unknown incline, rather than force it to run.

| Time | Duration | Stage | Speed km/h | Grade percent |
|---|---|---|---|---|
| 00:00–03:00 | 180 s | Warm-up | 2.0 → 2.6 | 0 |
| 03:00–06:00 | 180 s | Progressive ascent | 2.6 → 3.0 | 0 → 3 |
| 06:00–09:00 | 180 s | First climb | 3.0 | 3 → 5 over first minute, then 5 |
| 09:00–11:00 | 120 s | Active recovery | 3.0 → 2.5 over first minute, then 2.5 | 5 → 1 over first minute, then 1 |
| 11:00–14:00 | 180 s | Second climb | 2.5 → 3.2 over first minute, then 3.2 | 1 → 5 over first minute, then 5 |
| 14:00–16:00 | 120 s | Easier walking | 3.2 → 2.6 | 5 → 1 |
| 16:00–17:00 | 60 s | Cooldown descent | 2.6 → 2.4 | 1 → 0 |
| 17:00–20:00 | 180 s | Cooldown walking | 2.4 → 1.5 | 0 |
| After 20:00 | Device-dependent | Verified stop | Stop operation | Hold attained grade; no extra incline command |

Sum is 1,200 seconds of planned active time. Stop verification adds wall time. Ramp subrows remain separate durations in XML where a first-minute transition and a later hold are required. Slightly lower first/second climb speeds than the prompt example and gradual grade transitions make the prototype easier to scale; they do not establish beginner suitability for all users. Full 11% grade and loaded/backpack walking are excluded from the initial example.

Illustrative CLI XML fragment, showing proposed extensions and existing elements:

```xml
<rows version="1" id="uphill-walk-20" name="Uphill walking"
      speedunit="km/h" inclineunit="percent">
  <row name="Warm-up" duration="00:03:00" speedfrom="2.0" speedto="2.6"
       inclination="0" forcespeed="1">
    <textevent timeoffset="150" message="Ascent begins in 30 seconds"/>
    <textevent timeoffset="170" message="Ascent begins in 10 seconds"/>
  </row>
  <row name="Progressive ascent" duration="00:03:00"
       speedfrom="2.6" speedto="3.0" inclinationfrom="0" inclinationto="3"
       forcespeed="1"/>
  <!-- Remaining stages follow the table; this fragment is not a complete plan. -->
</rows>
```

M3 delivers a complete external example XML and a second different short workout; no stages, identifiers or discipline-specific logic belong in C++.

## Mapping and timing

Workout grades remain hardware independent. Preflight maps every constant/endpoint and every distinct quantized ramp target to a validated profile table. Use conservative mapping to an attained supported grade at or below the requested grade, show both in the summary, and require confirmation of any adjustment. Reject if there is no acceptable mapped value within a configured tolerance; never extrapolate eight settings to 11%. Speed rounds only to validated resolution within approved limits; inspect final bytes for overflow. User safety ceilings and reductions may lower targets, never expand the envelope.

Elapsed workout time starts only after fresh raw status supports running and physical-motion observation semantics were validated in M1. Initial warm-up speed must be reached within a startup deadline. Monotonic elapsed time drives both stages and warnings; UTC is for logs only. Start/countdown/connection time is excluded. Stage durations include planned ramps and expected hardware lag; display pending versus observed targets throughout. Do not hold every stage until its final ramp endpoint before starting its duration.

At each wake compute the target pair from current stage elapsed time. Quantize/coalesce, enforce tested slew limits and send only changed pairs; permit at most one normal in-flight command. Keep latest unsent pair, not a backlog of ramp samples. Within expected settling time track hardware lag; if target cannot be observed by its deadline, suspend automation and enter the fault/stop policy. Never advance through multiple missed stages and submit a burst of catch-up acceleration. An event-loop gap above the verified control budget aborts; OS suspend invalidates the session on return. Active pauses stop elapsed progression; disconnect/abort permanently ends this run.

Exact mathematical ramp endpoint is included even when it is not on a periodic tick. Quantization resolution and allowed control cadence determine wire targets; progress is independent of the number of writes. Avoid simultaneous aggressive speed/grade increases through conservative profile slew limits. Completion always enters STOPPING and waits for sufficient stop evidence; a zero speed target is not a substitute for the stop opcode.

## Notifications and manual input

For each logical transition generate 30-second, 10-second and transition-time messages containing next stage and resolved paired targets. Deduplicate by stage occurrence plus event ID, not just timestamp. Stages shorter than a warning lead time skip that warning. A delayed UI tick may combine due warnings without replaying an obsolete countdown. Explicit XML text events remain supported, including inside logical ramps; if they duplicate generated warnings, suppress by explicit notification identity. Final cooldown warnings describe the upcoming stop.

Optional instructions can suggest a comfortable conversational effort or a user-reported exertion value. No automatic HR control. Terminal bell is optional presentation after minimum functionality; speech adds dependencies and is deferred. Intensity reduction latches lower speed/grade ceilings and cancels pending increases; resume requires explicit confirmation after a validated hardware pause. Skip is outside v0.1.

## Training scope

This plan exercises controlled uphill walking and permits conservative progression; individual fitness, balance, equipment and tolerance remain unknown. It omits uneven surfaces, downhill movement/eccentric loading, outdoor technique, skiing skill, pack/equipment handling and environmental exposure. It cannot validate transfer to hiking or ski touring. M1/M3 targets demonstrate controller operation; workout suitability requires user-specific adjustment. Use optional perceived exertion instructions as feedback, with manual stop/reduction always available.
