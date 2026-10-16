# treadmill — run your own training plans on the RunPad W1

A headless CLI that plays an XML training plan on the RunPad W1 (FitShow
`FS-0F7A32`): it starts the belt, sets speed and incline on schedule, and stops
at the end. Everything runs locally; there's no cloud service and no phone app.
It runs under WSL and uses Windows Bluetooth through PowerShell. WSL itself has
no Bluetooth adapter.

Engineering history, protocol notes and experiment records are in
[ENGINEERING.md](ENGINEERING.md), [STATUS.md](STATUS.md) and
`docs/treadmill/HANDOFF.md`.

## Run a plan

```sh
treadmill workout list                       # plans in the catalog
treadmill workout run uphill-walk-20 --dry-run   # show the exact target schedule, no Bluetooth
treadmill workout run uphill-walk-20         # real run
treadmill workout run ~/plans/my-plan.xml    # any XML file path works too
```

Before a real run:

1. Wake the treadmill from its console. It sleeps after about 5 minutes idle and
   the CLI refuses to start a sleeping treadmill.
2. Keep the safety key attached, step on, and keep the physical stop within reach.
3. Type `MOTOR` when prompted.

The run starts at 1.0 km/h and moves to the first target once the belt reports
running. Each target is held after the treadmill reports it. At the end the
CLI sends STOP and waits for the treadmill to report that it's stopped. Every run
writes a JSON Lines log to `~/treadmill-logs/<plan>-<timestamp>.jsonl`
(`--log <file>` overrides it). `summarize_run.py <log>` prints a summary.

To end a run early, press `Q` or `S` or Ctrl-C. These send STOP. The
treadmill's own stop button always works too. **There is no pause.** The W1
ignores the FitShow pause command.

If the Bluetooth link drops, the CLI cannot deliver STOP. In that case use the
physical stop.

## Write a plan

Put plans in `tools/treadmill/workouts/` so they appear in `workout list`, or
pass a file path.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<rows version="1" id="my-hike-30" name="Hike 30" speedunit="km/h" inclineunit="percent" device="treadmill">
  <row name="Warm-up"  duration="00:05:00" speedfrom="2.0" speedto="3.0" inclination="0" forcespeed="1"/>
  <repeat times="3">
    <row name="Climb"   duration="00:05:00" speed="3.5" inclination="8.25" forcespeed="1"/>
    <row name="Recover" duration="00:02:00" speed="3.0" inclination="1.375" forcespeed="1"/>
  </repeat>
  <row name="Cooldown" duration="00:04:00" speedfrom="3.0" speedto="2.0" inclinationfrom="2.75" inclinationto="0" forcespeed="1"/>
</rows>
```

The format has these rules:

- The root element needs `version="1"`, a unique `id`, a `name`, `speedunit="km/h"`
  and `inclineunit="percent"`.
- Every row needs a `name`, a `duration` in `hh:mm:ss` and `forcespeed="1"`.
- Use either a constant `speed` or a ramp with `speedfrom` and `speedto`.
  Incline works the same way: `inclination`, or `inclinationfrom` and
  `inclinationto`.
- Ramps play as 10-second steps.
- `<repeat times="N">` can wrap rows. Repeats can't be nested.
- A plan can be up to 70 minutes long, with up to 512 distinct speed/incline
  changes.

### W1 limits and incline levels

- **Speed:** 1.0–6.0 km/h in 0.1 steps. The CLI refuses anything outside that
  range.
- **Incline:** the W1 has levels 0–8. Each grade in a plan is rounded to the
  nearest level using the mapping below.

| Level | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| Grade % | 0 | 1.375 | 2.75 | 4.125 | 5.5 | 6.875 | 8.25 | 9.625 | 11 |

This mapping is an **estimate** taken from the advertised 0–11% range with equal
steps. It is not measured. The speed unit (raw 10 = 1.0 km/h) matches the
console but has not been checked independently.

## Build

You need a C++17 compiler, plus Qt 5.15 Core, Bluetooth and Positioning
development packages. On Debian/Ubuntu install `build-essential qtbase5-dev
qt5-qmake qtconnectivity5-dev qtpositioning5-dev`.

```sh
mkdir -p /tmp/treadmill-build && cd /tmp/treadmill-build
qmake /home/mix/qdomyos-zwift/tools/treadmill/treadmill.pro
make -j4
```

WSL interop must work, meaning `powershell.exe` must run from WSL. If it fails
with `Exec format error`, the WSL interop registration has been removed (often
by `systemd-binfmt`). To restore it:

```sh
sudo sh -c 'echo ":WSLInterop:M::MZ::/init:PF" > /proc/sys/fs/binfmt_misc/register'
echo ':WSLInterop:M::MZ::/init:PF' | sudo tee /etc/binfmt.d/WSLInterop.conf   # keeps it after restarts
```

## Known issues

- **Sleep:** the W1 sleeps after about 5 minutes idle, and only a physical wake
  brings it back. Status polling doesn't keep it awake, and the FitShow READY
  command starts a countdown instead of waking it.
- **Unexplained STOPPING:** on 2026-10-14 the treadmill once reported
  "decelerating" mid-run without a host STOP. Repeat tests haven't reproduced
  it. If it happens, the CLI ends the run. Start the plan again; there's no resume.
- **Other options:** the CLI also has a Linux BLE backend (diagnostics only),
  a simulator (`--simulate`), and prepared control trials. Those came from the
  investigation and aren't needed for normal use. See ENGINEERING.md.
