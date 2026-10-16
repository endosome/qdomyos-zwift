# Nonmotor hardware checkpoint

Two operator-approved 20-second Windows probes completed with successful CCCD cleanup.
Only 5002, 5003 and 51 payloads were sent; no motor commands or pairing.

- Speed reply: 0250023c0a006403 (raw minimum 10, maximum 60, unit tag 0).
- Incline reply: 0250030800025903 (raw minimum 0, maximum 8, pause flag set).
- Magnet disconnected: 025106025503 (QZ SAFETY 6; extra byte 02 uninterpreted).
- Magnet normally attached: 0251005103 (QZ NORMAL 0; no motor telemetry).

Raw state/bounds are observations, not evidence of calibrated units, grade or
physical stop. Software revision 1.3.8 is not firmware identity. Hardware
workouts remain disabled. The later operator-controlled movement observation is summarized below;
software remained limited to nonmotor diagnostics.

Later 60-second observer run completed with successful notification cleanup.
It received 104 identical SAFETY replies (`025106025503`) and no speed/incline
telemetry. No motor commands were sent. The earlier NORMAL result after magnet
attachment was not repeatable; the physical safety state is unresolved. This
observation does not establish whether physical start/stop controls work.

After the operator woke the blank display with one physical Play/Pause press
(reported not to start the belt), a further 20-second observer completed with
successful cleanup. All 31 status replies were NORMAL (`0251005103`); speed and
incline bound replies were unchanged and no motion telemetry was received. No
motor commands were sent. The wake action coincided with NORMAL status, but the
trace does not establish causation or physical start/stop behavior.


During a later 60-second observer session, the operator manually started the
belt after a reported three-second countdown and pressed physical Play/Pause
after about three seconds of movement. Raw replies showed START countdown 3, 2,
1; RUNNING (0x03) with speed code 10 and incline code 0; then STOP (0x04) with
speed codes 9, 6, 4, 1; finally short NORMAL (0x00). No status-4 sample reported
zero speed. Operator reports the belt paused/stopped. This does not validate the
raw speed unit, incline mapping or verified physical zero. The Windows observer
sent only 5002, 5003 and 51 queries; cleanup succeeded. Software motor control
and the Linux Qt BLE backend remain unverified.

Latest 60-second observer run completed with successful cleanup but received
only 106 SAFETY replies and no speed or movement frames. The operator reported
that the physical console showed speed 1.0 and incline 0 after Play, then speed
falling by 0.1 steps to 0.0 after Pause; no unit was specified. Those readings
are not present in the BLE trace. Timing/correlation and the cause of the
SAFETY-only report remain unresolved.


A synchronized repeat observer captured RUNNING at raw speed code 10/incline
code 0 with the elapsed-seconds field counting 0–10, then STOP frames at raw
speed codes 10, 7, 4 and 1 at second 11, followed by short NORMAL status. It did
not receive a raw zero-speed frame. The operator's earlier report of display
speed 1.0 and incline 0 while running, then speed falling to 0.0 after Pause
(unit not reported), was not repeated for this capture, so the display-to-frame
numeric match remains unconfirmed. The 60-second observer completed and cleaned
up; only the three approved nonmotor queries were sent.


A further 60-second run intended to observe physical speed/incline step changes
returned limits max raw speed 120, min 10, unit tag 0x20, unlike the previous
max 60/tag 0 reply. Incline limits were unchanged. Status went NORMAL→START
(countdown 3/2/1)→RUNNING and remained raw speed 10/incline 0 through elapsed
second 12; no parameter changes or stop report were captured before timeout.
Cleanup succeeded. No motor commands were sent; post-capture physical state is
unknown.


After the latest capture, the operator reported the console speed was raised to
20.0 and lowered to 1.0; no unit was displayed. The observer RUNNING frames in
that capture remained at raw speed code 10 and incline code 0. Timing and the
display/BLE discrepancy are unresolved. Incline adjustment has not been tested.
