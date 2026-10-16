#!/usr/bin/env python3
"""Generate a W1 climb/recover training plan as XML, checked against the W1's limits.

The plan is: warm-up ramp, N repeats of (climb, recover), cooldown ramp. Incline is
given as W1 levels 0-8; each level is written as the working grade of 1.375 % per level
(0 % at level 0, the advertised 11 % at level 8), the same mapping the CLI uses.
With --check the CLI's own dry run validates the finished file (targets, deadline).
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from xml.sax.saxutils import quoteattr

GRADE_PER_LEVEL = 11.0 / 8.0
MIN_SPEED, MAX_SPEED = 1.0, 6.0
MAX_MINUTES = 70
W1_ADDRESS, W1_NAME = "DD:87:70:39:74:20", "FS-0F7A32"


def speed(value, label):
    if not MIN_SPEED <= value <= MAX_SPEED or abs(value * 10 - round(value * 10)) > 1e-9:
        raise ValueError(f"{label} {value} must be {MIN_SPEED}-{MAX_SPEED} km/h in 0.1 steps")
    return value


def level(value, label):
    if not 0 <= value <= 8:
        raise ValueError(f"{label} {value} must be a W1 incline level from 0 to 8")
    return value


def grade(value):
    text = f"{value * GRADE_PER_LEVEL:.3f}".rstrip("0").rstrip(".")
    return text or "0"


def duration(minutes, label):
    seconds = round(minutes * 60)
    if seconds < 10 or abs(minutes * 60 - seconds) > 1e-6:
        raise ValueError(f"{label} {minutes} min must be a whole number of seconds, at least 10")
    return seconds


def clock(seconds):
    return f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"


def build(args):
    if not args.id or not all(c.isalnum() or c in "-_" for c in args.id):
        raise ValueError("--id must be letters, digits, '-' or '_'")
    if not 1 <= args.repeats <= 40:
        raise ValueError("--repeats must be 1-40")
    warmup = duration(args.warmup, "--warmup")
    climb = duration(args.climb_minutes, "--climb-minutes")
    recover = duration(args.recover_minutes, "--recover-minutes")
    cooldown = duration(args.cooldown, "--cooldown")
    total = warmup + args.repeats * (climb + recover) + cooldown
    if total > MAX_MINUTES * 60:
        raise ValueError(f"plan is {total / 60:.1f} min; the limit is {MAX_MINUTES}")
    warm_from, warm_to = speed(args.warmup_from, "--warmup-from"), speed(args.warmup_to, "--warmup-to")
    climb_speed, recover_speed = speed(args.climb_speed, "--climb-speed"), speed(args.recover_speed, "--recover-speed")
    cool_to = speed(args.cooldown_to, "--cooldown-to")
    climb_level, recover_level = level(args.climb_level, "--climb-level"), level(args.recover_level, "--recover-level")
    if recover_level > climb_level:
        raise ValueError("--recover-level must not exceed --climb-level")
    name = args.name or f"{args.id} ({total // 60} min)"
    rows = [
        f'  <row name="Warm-up" duration="{clock(warmup)}" speedfrom="{warm_from}" speedto="{warm_to}" inclination="0" forcespeed="1"/>',
        f'  <repeat times="{args.repeats}">',
        f'    <row name="Climb" duration="{clock(climb)}" speed="{climb_speed}" inclination="{grade(climb_level)}" forcespeed="1"/>',
        f'    <row name="Recover" duration="{clock(recover)}" speed="{recover_speed}" inclination="{grade(recover_level)}" forcespeed="1"/>',
        '  </repeat>',
        f'  <row name="Cooldown" duration="{clock(cooldown)}" speedfrom="{recover_speed}" speedto="{cool_to}" '
        f'inclinationfrom="{grade(recover_level)}" inclinationto="0" forcespeed="1"/>',
    ]
    header = (f'<?xml version="1.0" encoding="UTF-8"?>\n<rows version="1" id={quoteattr(args.id)} name={quoteattr(name)} '
              f'speedunit="km/h" inclineunit="percent" device="treadmill">')
    return header + "\n" + "\n".join(rows) + "\n</rows>\n", total


def check(binary, path):
    result = subprocess.run([binary, "workout", "run", str(path), "--dry-run", "--backend", "linux",
                             "--device", W1_ADDRESS, "--expected-name", W1_NAME],
                            capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise ValueError("the CLI refused the plan: " + (result.stderr or result.stdout).strip())
    return json.loads(result.stdout)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--id", required=True)
    parser.add_argument("--name")
    parser.add_argument("--warmup", type=float, default=5, metavar="MIN")
    parser.add_argument("--warmup-from", type=float, default=2.0, metavar="KMH")
    parser.add_argument("--warmup-to", type=float, default=2.6, metavar="KMH")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--climb-minutes", type=float, default=4, metavar="MIN")
    parser.add_argument("--climb-speed", type=float, default=3.0, metavar="KMH")
    parser.add_argument("--climb-level", type=int, default=4, metavar="0-8")
    parser.add_argument("--recover-minutes", type=float, default=2, metavar="MIN")
    parser.add_argument("--recover-speed", type=float, default=2.5, metavar="KMH")
    parser.add_argument("--recover-level", type=int, default=1, metavar="0-8")
    parser.add_argument("--cooldown", type=float, default=5, metavar="MIN")
    parser.add_argument("--cooldown-to", type=float, default=1.5, metavar="KMH")
    parser.add_argument("--out", help="output file (default: print to stdout)")
    parser.add_argument("--check", metavar="TREADMILL", help="validate with this treadmill binary's dry run")
    args = parser.parse_args(argv)
    try:
        xml, total = build(args)
        if args.out:
            path = Path(args.out)
            if path.exists():
                raise ValueError(f"{path} already exists")
            path.write_text(xml, encoding="utf-8")
        else:
            sys.stdout.write(xml)
        if args.check:
            if not args.out:
                raise ValueError("--check needs --out")
            plan = check(args.check, args.out)
            print(f"{total // 60} min {total % 60} s; {plan['step_count']} targets; speed ceiling "
                  f"{plan['speed_ceiling_code'] / 10} km/h; incline ceiling level {plan['incline_ceiling_code']}; "
                  f"deadline {plan['windows_motor_deadline_seconds']} s", file=sys.stderr)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"make_plan: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
