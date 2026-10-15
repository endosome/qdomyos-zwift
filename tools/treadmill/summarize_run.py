#!/usr/bin/env python3
"""Summarize a treadmill JSON Lines run log without Bluetooth access."""

import argparse
import json
from pathlib import Path
import sys


FITSHOW_STATES = {
    0: "NORMAL",
    1: "END",
    2: "START/countdown",
    3: "RUNNING",
    4: "STOPPING",
    5: "ERROR",
    6: "SAFETY/disabled",
    7: "STUDY",
    9: "READY",
    10: "PAUSED",
}


def decode_treadmill_data(frame_hex):
    """Return FTMS instant speed (km/h) and inclination (%), when present."""
    try:
        data = bytes.fromhex(frame_hex)
    except (TypeError, ValueError):
        return None, None
    if len(data) < 2:
        return None, None

    flags = data[0] | (data[1] << 8)
    offset = 2
    speed = None
    inclination = None

    if flags & 0x0001:
        pass  # Instantaneous speed is absent when More Data is set.
    elif len(data) >= offset + 2:
        speed = (data[offset] | (data[offset + 1] << 8)) / 100.0
        offset += 2
    else:
        return None, None

    if flags & 0x0002:
        offset += 2  # Average speed.
    if flags & 0x0004:
        offset += 3  # Total distance.
    if flags & 0x0008:
        if len(data) >= offset + 2:
            raw = data[offset] | (data[offset + 1] << 8)
            if raw >= 0x8000:
                raw -= 0x10000
            inclination = raw / 10.0

    return speed, inclination


def read_json_lines(path):
    rows = []
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {error.msg}") from error
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: expected a JSON object")
            rows.append(row)
    return rows


def event_time(row, first_ms):
    value = row.get("monotonic_ms")
    if not isinstance(value, (int, float)):
        return "+??:??"
    seconds = max(0, int((value - first_ms) / 1000))
    return f"+{seconds // 60:02d}:{seconds % 60:02d}"


def describe_plan(row):
    trial = row.get("trial") or row.get("workout_id")
    name = row.get("workout_name")
    label = " / ".join(value for value in (trial, name) if value)
    if label:
        return label
    if row.get("event") == "trial_plan":
        return row.get("trial", "run")
    return "run"


def summarize(path):
    rows = read_json_lines(path)
    if not rows:
        raise ValueError(f"{path}: no log events")

    timed = [row.get("monotonic_ms") for row in rows if isinstance(row.get("monotonic_ms"), (int, float))]
    first_ms = min(timed) if timed else 0
    plan = next((row for row in rows if row.get("event") in ("trial_plan", "plan")), rows[0])
    print(f"\n{path}")
    print(f"Run: {describe_plan(plan)}")
    if plan.get("device") or plan.get("expected_name"):
        print(f"Device: {plan.get('expected_name', '?')} ({plan.get('device', '?')})")
    print("Timeline:")

    last_status = None
    last_ftms = {}
    last_ftms_status = None
    for row in rows:
        event = row.get("event", "")
        stamp = event_time(row, first_ms)
        if event == "module_identity":
            print(f"  {stamp} FitShow module {row.get('manufacturer', '?')}/{row.get('model', '?')} software {row.get('software_revision', '?')}")
        elif event in ("start_requested", "ready_requested"):
            print(f"  {stamp} {event.replace('_', ' ')} raw speed {row.get('speed_code', '?')}, incline {row.get('incline_code', '?')}")
        elif event == "reported_running":
            print(f"  {stamp} treadmill reported RUNNING")
        elif event in ("target_requested", "reported_target"):
            verb = "requested" if event == "target_requested" else "reported"
            speed = row.get("speed_code", "?")
            incline = row.get("incline_code", "?")
            stage = row.get("stage_name") or row.get("stage")
            suffix = f" ({stage})" if stage is not None else ""
            print(f"  {stamp} target {verb}: raw speed {speed}, incline {incline}{suffix}")
        elif event == "raw_status":
            state = row.get("status_code")
            motor = (row.get("speed_code"), row.get("incline_code")) if row.get("has_motor_data") else None
            key = (state, motor)
            if key != last_status:
                name = FITSHOW_STATES.get(state, f"state {state}")
                details = f", raw speed {motor[0]}, incline {motor[1]}" if motor else ""
                print(f"  {stamp} FitShow {name}{details}")
                last_status = key
        elif event == "ftms_notification":
            characteristic = str(row.get("characteristic_uuid", "")).lower()
            frame_hex = row.get("frame_hex", "")
            if characteristic.startswith("00002acd-"):
                speed, incline = decode_treadmill_data(frame_hex)
                values = (speed, incline)
                if values != last_ftms.get("2acd"):
                    details = []
                    if speed is not None:
                        details.append(f"speed {speed:.2f} km/h")
                    if incline is not None:
                        details.append(f"incline {incline:.1f}%")
                    print(f"  {stamp} FTMS reports {', '.join(details) if details else 'Treadmill Data without decoded speed/incline'}")
                    last_ftms["2acd"] = values
            elif characteristic.startswith("00002ada-"):
                normalized = frame_hex.upper()
                if normalized != last_ftms_status:
                    print(f"  {stamp} FTMS Fitness Machine Status raw {normalized or '(empty)'}")
                    last_ftms_status = normalized
        elif event in ("stop_requested", "fault"):
            print(f"  {stamp} {event.replace('_', ' ')}: {row.get('reason', '?')}")
        elif event == "trial_result":
            print(f"  {stamp} result: sequence_observed={row.get('sequence_observed')}, "
                  f"stop_write_succeeded={row.get('stop_write_succeeded')}, "
                  f"normal_status_after_stop={row.get('normal_status_after_stop')}, "
                  f"physical_stop_verified={row.get('physical_stop_verified')}")
        elif event == "windows_report":
            print(f"  {stamp} Windows cleanup: FitShow={row.get('notification_cleanup_status', '?')}, "
                  f"FTMS={row.get('ftms_observation_status', 'not requested')}")


def collect_paths(arguments):
    paths = []
    for value in arguments:
        path = Path(value)
        if path.is_dir():
            paths.extend(sorted(path.rglob("*.jsonl")))
        else:
            paths.append(path)
    return paths


def main(argv=None):
    parser = argparse.ArgumentParser(description="Summarize treadmill JSON Lines logs offline.")
    parser.add_argument("logs", nargs="+", help="JSON Lines log files or directories containing them")
    arguments = parser.parse_args(argv)
    paths = collect_paths(arguments.logs)
    if not paths:
        parser.error("no JSON Lines logs found")

    failed = False
    for path in paths:
        try:
            summarize(path)
        except (OSError, ValueError) as error:
            print(error, file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
