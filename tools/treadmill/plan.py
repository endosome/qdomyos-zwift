#!/usr/bin/env python3
"""Multi-week training plans made of FIT trainings: check, show and catalog.

A plan is a JSON manifest (tools/treadmill/plans/*.json) listing, per week, which
training (a .fit workout in tools/treadmill/workouts) is done on which day. Training
facts (minutes, estimated vertical gain, hard or not) are read from the FIT files with
treadmill-fit, so the plans cannot drift from the trainings. A training is "hard" when
it has any interval-intensity step.

  plan.py check <plan.json>...     validate; exit 1 on any problem
  plan.py show <plan.json> [--week N]
  plan.py catalog                  markdown table of every training
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
MAX_LOAD_JUMP = 1.2          # weekly minutes vs the larger of the previous two weeks
MAX_HARD_PER_WEEK = 2
MAX_TRAINING_DAYS = 5
PLAN_KEYS = {"id", "name", "goal", "audience", "weeks"}
WEEK_KEYS = {"week", "focus", "sessions", "note"}
SESSION_KEYS = {"day", "training", "off", "note"}


class Trainings:
    def __init__(self, workouts, tool):
        self.workouts, self.tool, self.cache = Path(workouts), tool, {}

    def ids(self):
        return sorted(path.stem for path in self.workouts.glob("*.fit"))

    def get(self, training_id):
        if training_id not in self.cache:
            path = self.workouts / (training_id + ".fit")
            if not path.exists():
                self.cache[training_id] = None
            else:
                result = subprocess.run([self.tool, "dump", str(path)], capture_output=True, text=True, timeout=30)
                if result.returncode != 0:
                    raise SystemExit(f"{path}: {result.stderr.strip()}")
                self.cache[training_id] = summarize(json.loads(result.stdout))
        return self.cache[training_id]


def flat_steps(nodes):
    for node in nodes:
        if "repeat" in node:
            for _ in range(node["repeat"]):
                yield from node["steps"]
        else:
            yield node


def summarize(dump):
    steps = list(flat_steps(dump["steps"]))
    return {"name": dump["name"], "description": dump.get("description", ""), "sport": dump["sport"],
            "minutes": dump["total_seconds"] / 60.0, "vertical": dump["vertical_meters_estimate"],
            "hard": any(step["intensity"] == "interval" for step in steps),
            "peak_speed": max(step["speed"] for step in steps), "peak_level": max(step["level"] for step in steps)}


def load_plan(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8")), []
    except (OSError, ValueError) as error:
        return None, [f"{path}: cannot read plan: {error}"]


def week_load(week, trainings):
    minutes = vertical = 0
    for session in week.get("sessions", []):
        info = trainings.get(session["training"]) if "training" in session else None
        if info:
            minutes += info["minutes"]
            vertical += info["vertical"]
    return minutes, vertical


def check(plan, trainings, label="plan"):
    errors = []
    def bad(message):
        errors.append(f"{label}: {message}")
    if not isinstance(plan, dict):
        return [f"{label}: the plan must be a JSON object"]
    for key in sorted(set(plan) - PLAN_KEYS):
        bad(f"unknown key '{key}'")
    for key in ("id", "name", "goal", "weeks"):
        if not plan.get(key):
            bad(f"missing '{key}'")
    weeks = plan.get("weeks") if isinstance(plan.get("weeks"), list) else []
    if len(weeks) < 4:
        bad("a plan needs at least 4 weeks")
    loads, hard_days = [], []
    for index, week in enumerate(weeks):
        where = f"week {index + 1}"
        if not isinstance(week, dict):
            bad(f"{where}: expected an object")
            continue
        for key in sorted(set(week) - WEEK_KEYS):
            bad(f"{where}: unknown key '{key}'")
        if week.get("week") != index + 1:
            bad(f"{where}: 'week' must be {index + 1}")
        if not week.get("focus"):
            bad(f"{where}: missing 'focus'")
        sessions = week.get("sessions") if isinstance(week.get("sessions"), list) else []
        last_day, training_days, hard_count = -1, 0, 0
        for session in sessions:
            if not isinstance(session, dict):
                bad(f"{where}: a session must be an object")
                continue
            for key in sorted(set(session) - SESSION_KEYS):
                bad(f"{where}: unknown session key '{key}'")
            day = session.get("day")
            if day not in DAYS:
                bad(f"{where}: day must be one of {', '.join(DAYS)}")
                continue
            if DAYS.index(day) <= last_day:
                bad(f"{where}: days must be in order and unique ({day})")
            last_day = DAYS.index(day)
            if ("training" in session) == ("off" in session):
                bad(f"{where} {day}: give exactly one of 'training' or 'off'")
                continue
            if "off" in session:
                continue
            training_days += 1
            info = trainings.get(session["training"])
            if info is None:
                bad(f"{where} {day}: unknown training '{session['training']}'")
                continue
            if "UNPROVEN LENGTH" in info["description"]:
                bad(f"{where} {day}: '{session['training']}' has an unproven length; keep it out of plans until that length has run")
            if info["hard"]:
                hard_count += 1
                hard_days.append(index * 7 + DAYS.index(day))
        if training_days == 0:
            bad(f"{where}: no training sessions")
        if training_days > MAX_TRAINING_DAYS:
            bad(f"{where}: more than {MAX_TRAINING_DAYS} training days leaves too little rest")
        if hard_count > MAX_HARD_PER_WEEK:
            bad(f"{where}: {hard_count} hard sessions; the limit is {MAX_HARD_PER_WEEK}")
        loads.append(week_load(week, trainings)[0])
    for earlier, later in zip(hard_days, hard_days[1:]):
        if later - earlier == 1:
            bad(f"hard sessions on consecutive days (week {earlier // 7 + 1} {DAYS[earlier % 7]} and {DAYS[later % 7]})")
    for index in range(1, len(loads)):
        ceiling = MAX_LOAD_JUMP * max(loads[max(0, index - 2):index])
        if loads[index] > ceiling + 1e-9:
            bad(f"week {index + 1}: {loads[index]:.0f} min is more than {round((MAX_LOAD_JUMP - 1) * 100)}% above the previous weeks "
                f"(limit {ceiling:.0f} min)")
    if len(loads) >= 2 and loads[-1] > loads[-2] + 1e-9:
        bad("the last week must not be heavier than the one before it (taper)")
    return errors


def show(plan, trainings, only_week=None):
    print(f"{plan['name']} ({plan['id']})")
    print(plan.get("goal", ""))
    if plan.get("audience"):
        print("For: " + plan["audience"])
    for week in plan["weeks"]:
        if only_week and week["week"] != only_week:
            continue
        minutes, vertical = week_load(week, trainings)
        print(f"\nWeek {week['week']}: {week['focus']}  ({minutes:.0f} min on the treadmill, about {vertical:.0f} m vertical, estimate)")
        if week.get("note"):
            print("  " + week["note"])
        for session in week["sessions"]:
            if "off" in session:
                print(f"  {session['day']}  {session['off']}")
                continue
            info = trainings.get(session["training"])
            tag = "HARD" if info["hard"] else "easy/steady"
            print(f"  {session['day']}  {info['name']} - {info['minutes']:.0f} min, ~{info['vertical']:.0f} m, {tag}")
            if session.get("note"):
                print(f"       {session['note']}")
            print(f"       treadmill workout run {session['training']} --wait-wake 10 --wait-wake-confirm")


def catalog(trainings):
    lines = ["| Training | Sport | Minutes | Vertical (est.) | Top speed | Top level | Effort |", "|---|---|---|---|---|---|---|"]
    for training_id in trainings.ids():
        info = trainings.get(training_id)
        lines.append(f"| `{training_id}` | {info['sport']} | {info['minutes']:.0f} | ~{info['vertical']:.0f} m | {info['peak_speed']:.1f} km/h | "
                     f"{info['peak_level']} | {'hard' if info['hard'] else 'easy/steady'} |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["check", "show", "catalog"])
    parser.add_argument("plans", nargs="*")
    parser.add_argument("--workouts", default=str(HERE / "workouts"))
    parser.add_argument("--fit-tool", default=os.environ.get("TREADMILL_FIT", "treadmill-fit"))
    parser.add_argument("--week", type=int)
    args = parser.parse_args(argv)
    trainings = Trainings(args.workouts, args.fit_tool)
    if args.command == "catalog":
        print(catalog(trainings))
        return 0
    if not args.plans:
        parser.error(f"{args.command} needs at least one plan file")
    status = 0
    for path in args.plans:
        plan, errors = load_plan(path)
        errors += check(plan, trainings, Path(path).name) if plan is not None else []
        for error in errors:
            print(error, file=sys.stderr)
        if errors:
            status = 1
        elif args.command == "show":
            show(plan, trainings, args.week)
        else:
            weeks = plan["weeks"]
            print(f"{Path(path).name}: ok, {len(weeks)} weeks")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
