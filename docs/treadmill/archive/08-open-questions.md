# Open questions and evidence gates

Baseline: `8b2e98ccc21cf28d3b946192094f4fb8af40d943`. These questions are **unverified** unless labeled confirmed. They are recorded for later implementation/hardware work; they do not prevent completing this planning task. No answer was silently assumed.

## P0 — Block automatic hardware workouts

| Question | Why it matters | Evidence / decision needed |
|---|---|---|
| What exact RunPad W1 variant and firmware is present? | App branding/specifications may span different hardware | Operator provides model label/manual; read-only identity report |
| What name, identity/address type and GATT layout does it expose? | Existing QZ patterns may miss or misclassify it | M1 scan/service report; explicit device selection |
| Does it actually speak the inspected FitShow framing and command set? | A connection or FitShow-app listing does not prove compatibility | Passive traces, reviewed query replies, gated controls |
| Are speed bytes km/h or mph, and what does UNIT mean? | QZ uses a setting rather than auto-detected conversion | Console/app/byte correlation and independent speed observation if available |
| What are valid speed limits/resolution and startup minimum? | QZ private defaults are not evidence | Protocol/manufacturer limits and conservative validation |
| Do raw status speed/grade represent attained motion/position or requested values? | QZ zeros nonrunning public speed; cannot infer physical stop | Low-intensity console/physical observations including deceleration |
| How do eight advertised incline settings map to raw values and percent grade? | Eight levels and 11% claim do not define scale | For current playback, use the operator-approved interpolation (code 0 = 0%, code 8 = 11%, 1.375 percentage points per level); treat it as estimated until independently calibrated |
| Is stop idempotent and sufficiently observable? What happens offline? | Stop retry and completion depend on this | Supervised start/stop traces and low-intensity interruption tests |
| Which initialization/authentication operations are required and safe? | Existing btinit writes user/date and can start | Official-app traces plus reviewed nonmotor allowlist |

If mandatory control capabilities or safe observation are unavailable, automatic W1 workout execution remains disabled. Diagnostics and simulation remain useful deliverables; full stated hardware acceptance is not achieved by those alone.

## P1 — Resolve before real controller acceptance

| Question | Current evidence | Follow-up |
|---|---|---|
| ACK means echo, accepted command or rejected command? | Confirmed QZ matches only cmd/subtype | Capture rejected/late responses; never use ACK as physical proof |
| Fragments/multiple frames per notification? | Confirmed QZ expects one complete frame | Capture real notification sizes before adding assembler |
| Multiple GATT families/FTMS/RSC exposed? | Confirmed QZ selects by discovery order and mutates FTMS setting | Explicit validated profile; optional-RSC test |
| Pause/resume semantics? | Constants and private pause flag exist; comments say to verify | Separate hardware tests; otherwise disable P/R |
| Multiple simultaneous controllers? | No exclusivity guarantee established | Close official app/QZ; test ownership separately if needed |
| Actual telemetry cadence, lag and safe retry budgets? | Default 200 ms polling does not prove W1 report cadence | Measure distributions; replace provisional deadlines |
| Can exact identity/firmware change after reset/power cycle? | Unknown | Profile invalidation policy and repeat discovery |
| Can FitShow link without homeform/virtual services? | Confirmed direct/transitive coupling; no build executed | M1.4 link spike and runtime isolation evidence |
| Does Linux backend require a particular permission/capability? | Not checked on this machine | Verify chosen distro/Qt backend; document least necessary privilege |

## P2 — Design recommendations awaiting implementation evidence

Option B is the planning recommendation; it is not an already proven minimal build. A failed link-isolation spike may justify extracting a bounded internal library earlier. Keep packet/GATT reuse and avoid an independent FitShow rewrite.

Reuse XML syntax with a strict standalone stage loader and additive units/metadata/incline ramps. Determine the smallest shared-parser change after fixtures demonstrate current behavior. Existing XML ramp endpoints, remainder durations and child event handling need tests; do not assert equivalence to intended linear ramps. Legacy QZ may ignore CLI extensions, so file compatibility must be documented precisely.

A steady clock is mandatory. Decide the actual maximum wake gap/suspend detector for the chosen Linux runtime. Proposed deadlines in [05](05-safety-and-state-machine.md) exist for deterministic simulation only until hardware measurement. Simulator feedback is deliberately richer than potentially available W1 feedback; keep capability flags honest when adapting real hardware.

Decide packaging after dependency/license inventory. Repository LICENSE is GPLv3 text; exact notices of retained components and permissible distribution choices need review, and library extraction cannot establish relicensing rights. No new SDK/license has been assumed.

## User input needed at later gates

1. Exact W1 model/variant/firmware, manufacturer instructions and access to the physical stop mechanism.
2. Linux host/adapter intended for the real controller and availability for read-only discovery.
3. Availability of console/official-app traces and whether existing pairing/controllers must be disconnected.
4. Explicit operator confirmation for each bounded initial motor procedure after capability validation; planning authorization does not grant motor-test authorization.
5. Conservative user speed/grade ceilings and tolerances for adapting generic grades to discrete calibrated settings. Default example targets are editable proposals, not personal fitness requirements.
6. Whether verified hardware pause is needed for v0.1 or can remain disabled. The minimum design already supports abort/stop.

No user health information, cloud accounts, trainer integration or backpack load is needed for repository planning. Questions about training suitability are separate from proving protocol behavior. Implementation scope changes should be discussed with the user before coding them.

## Decisions already concrete enough to implement

Executable name `treadmill`; Linux/C++17/Qt/qmake; separate target; read-only diagnostic first; no GUI/cloud startup; same asynchronous real/simulator contract; explicit motor confirmation; atomic paired targets; stop priority and generation cancellation; monotonic generic workout runner; retained XML dialect; separate device calibration; JSON Lines logs; eight planning documents only in this task. These are design recommendations grounded in the requested scope, not claims of existing functionality.
