#!/usr/bin/env python3
"""Terminal UI for the treadmill CLI: choose a training, run it and watch it.

The CLI does the work and keeps every safety check. This wrapper starts `treadmill workout run <id> --manual-control`
on a pty, passes the operator's ENTER presses through, sends stop, speed and incline commands as lines on stdin and
draws the JSON records the CLI writes to stdout (API.md, "Manual control"). If this program ends, the CLI's input
closes and the CLI stops the belt.

usage: treadmill_tui.py [PLAN_FILE ...]      extra .xml/.fit plans are listed next to the catalog
TREADMILL_BIN selects the CLI (default: `treadmill` on PATH); TREADMILL_LOG_DIR the log directory (~/treadmill-logs).
"""

import curses
import datetime
import json
import locale
import os
from pathlib import Path
import pty
import queue
import shutil
import subprocess
import sys
import textwrap
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # no __pycache__ next to the scripts
from summarize_run import decode_total_distance  # noqa: E402

HERE = Path(__file__).resolve().parent
LOG_DIR = Path(os.environ.get("TREADMILL_LOG_DIR") or Path.home() / "treadmill-logs")
GRADE_PER_LEVEL = 11.0 / 8.0  # provisional W1 mapping (README), not measured
FTMS_TREADMILL_DATA = "00002acd-0000-1000-8000-00805f9b34fb"
PROMPT_APPROVE, PROMPT_AGAIN = "Press ENTER to approve", "Press ENTER again"
CATEGORIES = [("all", "All"), ("hike", "Hiking"), ("mr", "Power-hike"), ("mrun", "Mountain run"), ("run", "Running"),
              ("trail", "Trail"), ("skimo", "Ski touring"), ("vo2", "VO2max"), ("file", "Files")]
STRATEGIES = ["hold", "offset", "takeover"]
STATES = {0: "IDLE", 1: "END", 2: "COUNTDOWN", 3: "RUNNING", 4: "STOPPING", 5: "ERROR", 6: "DISABLED", 7: "STUDY",
          9: "READY", 10: "PAUSED"}


def cli_path():
    return os.environ.get("TREADMILL_BIN") or shutil.which("treadmill") or "treadmill"


def clock(seconds):
    seconds = max(0, int(round(seconds)))
    hours, rest = divmod(seconds, 3600)
    return "%d:%02d:%02d" % (hours, rest // 60, rest % 60) if hours else "%d:%02d" % (rest // 60, rest % 60)


def base_name(name):
    """'Warm-up 3/6' -> 'Warm-up': ramp pieces share one segment name."""
    head, _, last = name.rpartition(" ")
    numbers = last.split("/")
    if head and len(numbers) == 2 and numbers[0].isdigit() and numbers[1].isdigit():
        return head
    return name


def segments(steps):
    """Group consecutive plan steps with the same base name: [{name, first, last, seconds, number, of}]."""
    result = []
    for index, step in enumerate(steps):
        name = base_name(step["name"])
        if result and result[-1]["name"] == name:
            result[-1]["last"] = index
            result[-1]["seconds"] += step["hold_seconds"]
        else:
            result.append({"name": name, "first": index, "last": index, "seconds": step["hold_seconds"]})
    for segment in result:
        same = [s for s in result if s["name"] == segment["name"]]
        segment["number"], segment["of"] = same.index(segment) + 1, len(same)
    return result


def plan_climb_m(steps):
    return sum(s["hold_seconds"] * s["speed_code"] / 36.0 * s["incline_code"] * GRADE_PER_LEVEL / 100 for s in steps)


# --- catalog -------------------------------------------------------------------------------------------------------

def category_of(ident, is_file):
    if is_file:
        return "file"
    prefix = ident.split("-")[0]
    return prefix if prefix in dict(CATEGORIES) else "all"


def last_runs():
    """Most recent log per plan id: {id: (datetime, completed or None)}."""
    latest = {}
    if not LOG_DIR.is_dir():
        return {}
    for path in LOG_DIR.glob("*.jsonl"):
        parts = path.stem.rsplit("-", 2)
        if len(parts) != 3 or not (parts[1].isdigit() and parts[2].isdigit() and len(parts[1]) == 8):
            continue
        if parts[0] not in latest or path.stem > latest[parts[0]].stem:
            latest[parts[0]] = path
    result = {}
    for ident, path in latest.items():
        completed = None
        try:
            with path.open("rb") as handle:
                handle.seek(max(0, path.stat().st_size - 8192))
                for line in handle.read().decode(errors="replace").splitlines():
                    if '"trial_result"' in line:
                        completed = json.loads(line).get("sequence_observed")
        except (OSError, ValueError):
            pass
        stamp = datetime.datetime.strptime(path.stem[-15:], "%Y%m%d-%H%M%S")
        result[ident] = (stamp, completed)
    return result


def load_catalog(cli, extra_files):
    entries = []
    try:
        listed = subprocess.run([cli, "workout", "list"], capture_output=True, text=True, timeout=30)
        entries = json.loads(listed.stdout) if listed.returncode == 0 else []
    except (OSError, ValueError, subprocess.TimeoutExpired):
        entries = []
    for path in extra_files:
        entries.append({"id": str(Path(path).resolve()), "name": Path(path).stem, "path": str(Path(path).resolve()),
                        "duration_seconds": None, "file": True})
    runs = last_runs()
    for entry in entries:
        entry["category"] = category_of(entry["id"], entry.get("file"))
        entry["description"] = ""
        spec = HERE / "trainings" / (entry["id"] + ".json")
        if not entry.get("file") and spec.is_file():
            try:
                entry["description"] = json.loads(spec.read_text()).get("description", "")
            except ValueError:
                pass
        entry["last_run"] = runs.get(Path(entry["id"]).stem if entry.get("file") else entry["id"])
    return entries


def dry_run(cli, ident, running):
    """The CLI's own plan for this training: (plan, None) or (None, the CLI's reason)."""
    arguments = [cli, "workout", "run", ident, "--dry-run"] + (["--running-mode"] if running else [])
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, str(error)
    if result.returncode == 0:
        try:
            return json.loads(result.stdout), None
        except ValueError:
            return None, "unreadable dry-run output"
    lines = [line for line in result.stderr.strip().splitlines() if line.strip()]
    return None, lines[-1] if lines else "dry run failed (exit %d)" % result.returncode


def export_activity(log_path, entry):
    """`treadmill-fit activity` for a finished run, written next to its log; returns a one-line result."""
    tool = os.environ.get("TREADMILL_FIT") or shutil.which("treadmill-fit")
    if not tool:
        return "treadmill-fit is not installed (tools/treadmill/fit)"
    sport = "walking"
    spec = HERE / "trainings" / (entry["id"] + ".json")
    if not entry.get("file") and spec.is_file():
        try:
            sport = {"hiking": "hiking", "running": "running"}.get(json.loads(spec.read_text()).get("sport"), "walking")
        except ValueError:
            pass
    out = log_path.with_suffix(".fit")
    try:
        result = subprocess.run([tool, "activity", str(log_path), str(out), "--sport", sport], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        return "FIT export failed: %s" % error
    if result.returncode != 0:
        return "FIT export failed: " + (result.stderr.strip().splitlines() or ["exit %d" % result.returncode])[-1]
    return "FIT activity written: %s (%s)" % (out, sport)


# --- the CLI child process -----------------------------------------------------------------------------------------

class Child:
    """The CLI on a pty (it insists on a terminal for stdin). stdout carries one JSON record per line and must always be
    read, or the helper stops the belt; reader threads drain stdout, stderr and the pty into one queue."""

    def __init__(self, arguments, env=None):
        self.master, slave = pty.openpty()
        self.process = subprocess.Popen(arguments, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        env=env, start_new_session=True)
        os.close(slave)
        self.events = queue.Queue()
        self.seq = 0
        self.finished = threading.Event()
        threading.Thread(target=self._stdout, daemon=True).start()
        threading.Thread(target=self._chunks, args=(self.process.stderr.fileno(), "stderr"), daemon=True).start()
        threading.Thread(target=self._chunks, args=(self.master, None), daemon=True).start()  # pty echo, discarded

    def _stdout(self):
        for raw in self.process.stdout:
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                self.events.put(("stdout", line))
                continue
            self.events.put(("record", record))
        self.events.put(("exit", self.process.wait()))
        self.finished.set()

    def _chunks(self, fd, kind):
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            if kind:
                self.events.put((kind, data.decode(errors="replace")))

    def send(self, text):
        try:
            os.write(self.master, text.encode())
            return True
        except OSError:
            return False

    def command(self, cmd):
        self.seq += 1
        self.send(json.dumps({"seq": self.seq, "cmd": cmd}) + "\n")
        return self.seq

    def stop(self):
        self.send("q\n")  # a bare q cancels at the approval prompts and stops the belt at any later point

    def close(self, timeout=30):
        """Stop the run if it is still going, keep draining until the CLI exits, then close its input."""
        if self.process.poll() is None:
            self.stop()
            self.finished.wait(timeout)
        try:
            os.close(self.master)
        except OSError:
            pass
        if self.process.poll() is None:
            self.process.wait(5)
        self.process.stdout.close()
        self.process.stderr.close()


class StderrParser:
    """The CLI's human-readable stderr: whole lines, minus the pretty-printed plan JSON, and its two approval prompts,
    which end in ': ' without a newline. `prompts` counts the prompts printed so far."""

    def __init__(self):
        self.tail, self.in_json, self.prompts = "", False, 0

    def feed(self, text):
        self.tail = self._cut_prompts(self.tail + text)
        *lines, self.tail = self.tail.split("\n")
        shown = []
        for line in lines:
            if line == "{":
                self.in_json = True
            if self.in_json:
                self.in_json = line != "}"
                continue
            if line.strip():
                shown.append(line)
        return shown

    def _cut_prompts(self, text):
        for prompt in (PROMPT_APPROVE, PROMPT_AGAIN):
            start = text.find(prompt)
            end = text.find(": ", start) if start >= 0 else -1
            if end >= 0:
                self.prompts += 1
                text = text[:start] + text[end + 2:]
        return text


# --- run state, built only from the CLI's records -------------------------------------------------------------------

class RunState:
    """Everything the run screen shows, derived from the CLI's JSON records. Times are the CLI's monotonic_ms."""

    def __init__(self):
        self.steps, self.segments, self.name, self.profile = [], [], "", "walking"
        self.plan = {}
        self.phase = "connecting"
        self.status = self.disabled_reason = None
        self.status_at = None
        self.speed = self.incline = 0
        self.target_speed, self.target_incline = 10, 0
        self.stage = 0
        self.step_start = {}
        self.frozen_at = None  # plan clock frozen (console pause or the ramp back)
        self.paused_ms = 0  # plan-clock time lost to pauses and ramps
        self.pause_started = None
        self.idle_ms = 0  # console-paused time (what the 15-minute allowance counts)
        self.pauses = 0
        self.running_at = None
        self.grace_until = None
        self.distance_m = self.climb_m = 0.0
        self._last_motion = None
        self.ftms_m = None
        self._ftms_start = self._ftms_last = None
        self._ftms_done = 0
        self.reported_distance_m = None
        self.key_removed = None  # (monotonic_ms, utc) of the first safety-key report
        self.faults, self.stop_reason, self.result = [], None, None
        self.messages = []  # (monotonic_ms or None, level, text)
        self.pause_supported = None
        self.pending = {}  # seq -> cmd awaiting a result
        self.last_ms = 0
        self.max_speed = 0

    def say(self, level, text, at=None):
        self.messages.append((at, level, text))
        del self.messages[:-200]

    # Plan clock -----------------------------------------------------------------------------------------------------
    def step_elapsed_ms(self, now):
        start = self.step_start.get(self.stage)
        if start is None:
            return 0
        return (self.frozen_at if self.frozen_at is not None else now) - start

    def plan_elapsed_ms(self, now):
        done = sum(s["hold_seconds"] for s in self.steps[:self.stage]) * 1000
        return done + min(self.step_elapsed_ms(now), self.steps[self.stage]["hold_seconds"] * 1000 if self.steps else 0)

    def plan_total_ms(self):
        return sum(s["hold_seconds"] for s in self.steps) * 1000

    def segment_index(self):
        for index, segment in enumerate(self.segments):
            if segment["first"] <= self.stage <= segment["last"]:
                return index
        return 0

    def segment_left_ms(self, now):
        if not self.segments:
            return 0
        segment = self.segments[self.segment_index()]
        rest = sum(s["hold_seconds"] for s in self.steps[self.stage:segment["last"] + 1]) * 1000
        return max(0, rest - self.step_elapsed_ms(now))

    def step_left_ms(self, now):
        if not self.steps:
            return 0
        return max(0, self.steps[self.stage]["hold_seconds"] * 1000 - self.step_elapsed_ms(now))

    def paused_now(self):
        return self.phase in ("paused", "resuming")

    def pause_allowance_left_s(self, now):
        allowance = self.plan.get("console_pause_allowance_seconds") or 0
        idle = self.idle_ms + (now - self.pause_started if self.phase == "paused" and self.pause_started is not None else 0)
        return allowance - idle / 1000.0

    def moving_ms(self, now):
        if self.running_at is None:
            return 0
        end = now if self.result is None else self.last_ms
        return max(0, end - self.running_at - self.idle_ms -
                   (end - self.pause_started if self.phase == "paused" and self.pause_started is not None else 0))

    def distance(self):
        """(metres, source): the treadmill's own counter when it is reported, otherwise speed integrated over time."""
        if self.reported_distance_m is not None:
            return self.reported_distance_m, "treadmill"
        if self.ftms_m is not None:
            return self.ftms_m, "treadmill"
        return self.distance_m, "estimate"

    # Records --------------------------------------------------------------------------------------------------------
    def feed(self, record):
        event = record.get("event")
        at = record.get("monotonic_ms", self.last_ms)
        self.last_ms = max(self.last_ms, at)
        handler = getattr(self, "on_" + str(event), None)
        if handler:
            handler(record, at)

    def on_trial_plan(self, r, at):
        self.plan = r
        self.steps = r.get("workout_steps") or []
        self.segments = segments(self.steps)
        self.name, self.profile = r.get("workout_name") or r.get("trial", ""), r.get("profile", "walking")

    def on_transport_ready(self, r, at):
        self.phase = "preflight"
        self.say("info", "Connected to %s." % r.get("name", "the treadmill"), at)

    def on_waiting_for_wake(self, r, at):
        self.phase = "asleep"

    def on_wake_lost(self, r, at):
        self.phase, self.grace_until = "asleep", None

    def on_awake_waiting_for_confirmation(self, r, at):
        self.phase = "awake"

    def on_wake_grace_started(self, r, at):
        self.phase, self.grace_until = "grace", at + r.get("grace_ms", 5000)

    def on_start_confirmed(self, r, at):
        self.phase = "preflight"
        self.say("info", "Start confirmed.", at)

    def on_start_requested(self, r, at):
        self.phase = "starting"
        self.say("info", "Starting the belt at 1.0 km/h.", at)

    def on_reported_running(self, r, at):
        self.phase, self.running_at = "running", at
        self.say("good", "The belt is running.", at)

    def on_raw_status(self, r, at):
        code, reason = r.get("status_code"), r.get("disabled_reason_code")
        self.status, self.disabled_reason, self.status_at = code, reason, at
        if r.get("has_motor_data"):
            self.speed, self.incline = r.get("speed_code", 0), r.get("incline_code", 0)
        if code == 6 and reason == 1 and self.key_removed is None:
            self.key_removed = (at, r.get("utc"))
            self.say("danger", "SAFETY KEY REMOVED. The console stops the belt and the CLI ends the run.", at)
        if code == 2 and self.phase == "starting":
            self.phase = "countdown"
        # Distance and climb: the previous sample's speed over the time since it.
        moving = code == 3 and r.get("has_motor_data")
        if self._last_motion is not None and self.running_at is not None:
            then, speed, incline = self._last_motion
            seconds = min(3.0, (at - then) / 1000.0)
            metres = speed / 36.0 * seconds
            self.distance_m += metres
            self.climb_m += metres * incline * GRADE_PER_LEVEL / 100
        self._last_motion = (at, self.speed, self.incline) if moving else None
        if moving:
            self.max_speed = max(self.max_speed, self.speed)

    def on_ftms_notification(self, r, at):
        if r.get("characteristic_uuid") != FTMS_TREADMILL_DATA or self.running_at is None:
            return
        metres = decode_total_distance(r.get("frame_hex", ""))
        if metres is None:
            return
        # The counter restarts at zero after a console pause; a falling value starts a new segment (as the CLI does).
        if self._ftms_start is None:
            self._ftms_start = metres
        if self._ftms_last is not None and metres < self._ftms_last:
            self._ftms_done += self._ftms_last - self._ftms_start
            self._ftms_start = 0
        self._ftms_last = metres
        self.ftms_m = max(self.ftms_m or 0, self._ftms_done + metres - self._ftms_start)

    def on_target_requested(self, r, at):
        self.stage = r.get("stage", self.stage)
        self.target_speed, self.target_incline = r.get("speed_code", self.target_speed), r.get("incline_code", self.target_incline)

    def on_reported_target(self, r, at):
        stage = r.get("stage", self.stage)
        if stage not in self.step_start and self.frozen_at is None:
            self.step_start[stage] = at

    def on_manual_command_received(self, r, at):
        if "raw" in r:
            self.say("warn", "The CLI did not understand a command line.", at)

    def on_manual_command_result(self, r, at):
        cmd, result = r.get("cmd"), r.get("result")
        self.pending.pop(r.get("seq"), None)
        if cmd in ("pause", "resume"):
            if result == "refused" and r.get("reason") == "unknown command":
                self.pause_supported = False
                self.say("warn", "Pause from here is not in the CLI yet: press Pause/Play on the console.", at)
                return
            if result == "applied":
                self.pause_supported = True
                if cmd == "pause":
                    self.on_console_pause({"source": "host"}, at)
                else:
                    self.on_console_play({"source": "host"}, at)
                return
        if result == "applied" and "speed_code" in r:
            self.target_speed, self.target_incline = r["speed_code"], r["incline_code"]
            if cmd != "stop":
                self.say("info", "Target %.1f km/h, level %d (%s)." % (r["speed_code"] / 10, r["incline_code"], cmd.replace("_", " ")), at)
        elif result == "refused":
            self.say("warn", "%s refused: %s" % ((cmd or "command").replace("_", " "), r.get("reason", "")), at)

    def on_console_pause(self, r, at):
        if self.phase != "paused":
            self.pauses += 1
            self.pause_started = at
        if self.frozen_at is None:
            self.frozen_at = at
        self.phase = "paused"
        if r.get("source") == "host":  # the CLI prints its own line for a console pause
            self.say("warn", "Paused from here.", at)

    def on_console_play(self, r, at):
        if self.phase == "paused" and self.pause_started is not None:
            self.idle_ms += at - self.pause_started
            self.pause_started = None
        self.phase = "resuming"
        self.say("info", "Play: returning gradually to the plan.", at)

    def on_resume_target_requested(self, r, at):
        self.target_speed, self.target_incline = r.get("speed_code", self.target_speed), r.get("incline_code", self.target_incline)

    def on_console_resumed(self, r, at):
        if self.frozen_at is not None:
            # As the CLI does: the step continues where it was paused, or starts now if it had not been reached yet.
            if self.stage in self.step_start:
                self.step_start[self.stage] += at - self.frozen_at
            else:
                self.step_start[self.stage] = at
            self.paused_ms += at - self.frozen_at
            self.frozen_at = None
        self.phase = "running"
        self.target_speed, self.target_incline = r.get("speed_code", self.target_speed), r.get("incline_code", self.target_incline)

    on_host_pause = on_console_pause
    on_host_resume = on_console_play
    on_host_resumed = on_console_resumed

    def on_stop_requested(self, r, at):
        if self.phase == "paused" and self.pause_started is not None:
            self.idle_ms += at - self.pause_started
            self.pause_started = None
        self.phase, self.stop_reason = "stopping", r.get("reason")
        self.say("warn" if r.get("reason") != "Prepared observation holds complete" else "good", "Stopping: %s." % r.get("reason"), at)

    def on_fault(self, r, at):
        self.faults.append(r.get("reason", "fault"))
        self.say("danger", "Fault: %s" % r.get("reason"), at)

    def on_distance_reported(self, r, at):
        self.reported_distance_m = r.get("distance_m")

    def on_trial_result(self, r, at):
        self.result = r
        self.phase = "finished"

    def completed(self):
        return bool(self.result and self.result.get("sequence_observed")) and self.stop_reason == "Prepared observation holds complete"


# --- drawing -------------------------------------------------------------------------------------------------------

DIGITS = {
    "0": ["███", "█ █", "█ █", "█ █", "███"], "1": [" ██", "  █", "  █", "  █", "  █"],
    "2": ["███", "  █", "███", "█  ", "███"], "3": ["███", "  █", " ██", "  █", "███"],
    "4": ["█ █", "█ █", "███", "  █", "  █"], "5": ["███", "█  ", "███", "  █", "███"],
    "6": ["███", "█  ", "███", "█ █", "███"], "7": ["███", "  █", "  █", "  █", "  █"],
    "8": ["███", "█ █", "███", "█ █", "███"], "9": ["███", "█ █", "███", "  █", "███"],
    ".": ["   ", "   ", "   ", "   ", " ▄ "], ":": ["   ", " ▀ ", "   ", " ▀ ", "   "],
    "-": ["   ", "   ", "███", "   ", "   "], " ": ["   "] * 5,
}
BARS = " ▁▂▃▄▅▆▇█"


class Colors:
    def __init__(self):
        self.rich = curses.has_colors() and curses.COLORS >= 256
        curses.start_color()
        try:
            curses.use_default_colors()
            background = -1
        except curses.error:
            background = curses.COLOR_BLACK
        pick = (lambda fine, basic: fine if self.rich else basic) if curses.has_colors() else (lambda fine, basic: curses.COLOR_WHITE)
        table = {
            "title": (pick(16, curses.COLOR_BLACK), pick(39, curses.COLOR_CYAN)),
            "accent": (pick(45, curses.COLOR_CYAN), background), "good": (pick(42, curses.COLOR_GREEN), background),
            "warn": (pick(214, curses.COLOR_YELLOW), background), "danger": (pick(203, curses.COLOR_RED), background),
            "next": (pick(177, curses.COLOR_MAGENTA), background), "dim": (pick(244, curses.COLOR_WHITE), background),
            "big": (pick(231, curses.COLOR_WHITE), background), "info": (pick(252, curses.COLOR_WHITE), background),
            "badge_good": (pick(16, curses.COLOR_BLACK), pick(42, curses.COLOR_GREEN)),
            "badge_warn": (pick(16, curses.COLOR_BLACK), pick(214, curses.COLOR_YELLOW)),
            "badge_danger": (pick(231, curses.COLOR_WHITE), pick(160, curses.COLOR_RED)),
            "badge_info": (pick(16, curses.COLOR_BLACK), pick(45, curses.COLOR_CYAN)),
            "select": (pick(231, curses.COLOR_WHITE), pick(24, curses.COLOR_BLUE)),
        }
        self.pairs = {}
        for number, (name, (fg, bg)) in enumerate(table.items(), start=1):
            curses.init_pair(number, fg, bg)
            self.pairs[name] = curses.color_pair(number)
        # Incline levels 0-8 from green through yellow to red.
        ramp = [42, 78, 114, 149, 185, 221, 215, 209, 203] if self.rich else \
            [curses.COLOR_GREEN] * 3 + [curses.COLOR_YELLOW] * 3 + [curses.COLOR_RED] * 3
        for level, color in enumerate(ramp):
            curses.init_pair(40 + level, color, background)
            self.pairs["level%d" % level] = curses.color_pair(40 + level)

    def __getitem__(self, name):
        return self.pairs.get(name, 0)


class Screen:
    def __init__(self, window):
        self.window = window
        self.colors = Colors()

    def put(self, y, x, text, attribute=0, width=None):
        height, columns = self.window.getmaxyx()
        if y < 0 or y >= height or x >= columns:
            return
        limit = columns - x - (1 if y == height - 1 else 0)
        if width is not None:
            limit = min(limit, width)
        if limit <= 0:
            return
        try:
            self.window.addstr(y, x, text[:limit], attribute)
        except curses.error:
            pass

    def fill(self, y, attribute):
        _, columns = self.window.getmaxyx()
        self.put(y, 0, " " * columns, attribute)

    def big(self, y, x, text, attribute):
        for row in range(5):
            self.put(y + row, x, " ".join(DIGITS.get(ch, DIGITS[" "])[row] for ch in text), attribute)

    def box(self, y, x, height, width, title, attribute):
        self.put(y, x, "╭─ " + title + " " + "─" * max(0, width - len(title) - 5) + "╮", attribute)
        for row in range(1, height - 1):
            self.put(y + row, x, "│", attribute)
            self.put(y + row, x + width - 1, "│", attribute)
        self.put(y + height - 1, x, "╰" + "─" * (width - 2) + "╯", attribute)

    def bar(self, y, x, width, fraction, attribute, rest_attribute):
        filled = int(round(max(0.0, min(1.0, fraction)) * width))
        self.put(y, x, "━" * filled, attribute)
        self.put(y, x + filled, "─" * (width - filled), rest_attribute)

    def profile(self, y, x, width, height, steps, position_s=None):
        """Speed bars over time, coloured by incline level; the column at position_s is marked below the chart."""
        total = sum(s["hold_seconds"] for s in steps)
        if not steps or total <= 0 or width < 4:
            return
        top = max(s["speed_code"] for s in steps)
        low = min(s["speed_code"] for s in steps)
        floor = max(0, low - max(5, (top - low) // 2))  # bars start below the slowest step, so changes stand out
        columns = []
        for column in range(width):
            moment = (column + 0.5) * total / width
            passed = 0
            for step in steps:
                passed += step["hold_seconds"]
                if moment < passed:
                    columns.append(step)
                    break
            else:
                columns.append(steps[-1])
        marker = None if position_s is None else min(width - 1, int(position_s / total * width))
        for column, step in enumerate(columns):
            eighths = max(1, int(round((step["speed_code"] - floor) / (top - floor) * height * 8)))
            attribute = self.colors["level%d" % max(0, min(8, step["incline_code"]))]
            if marker is not None and column < marker:
                attribute |= curses.A_DIM
            for row in range(height):
                level = max(0, min(8, eighths - row * 8))
                if level:
                    self.put(y + height - 1 - row, x + column, BARS[level], attribute)
        if marker is not None:
            self.put(y + height, x + marker, "▲", self.colors["accent"] | curses.A_BOLD)
        self.put(y, x + width + 1, "%.1f" % (top / 10), self.colors["dim"])


# --- the application -----------------------------------------------------------------------------------------------

class App:
    def __init__(self, window, extra_files):
        self.screen = Screen(window)
        self.window = window
        self.cli = cli_path()
        self.extra_files = extra_files
        self.entries = load_catalog(self.cli, extra_files)
        self.category = 0
        self.cursor = 0
        self.search = None
        self.handrail_up = False
        self.wake = True
        self.strategy = 0
        self.plans = {}
        self.mode = "picker"
        self.child = None
        self.state = None
        self.offset_ms = None
        self.briefing, self.stderr = [], StderrParser()
        self.prompt = 0
        self.approved = 0
        self.cancel_requested = False
        self.log_path = None
        self.run_entry = self.run_plan = None
        self.run_running = False
        self.export_message = ""
        self.exit_code = None
        self.beeps = True
        self.alerted = set()
        self.flash_until = 0
        self.message = ""

    # Picker -----------------------------------------------------------------------------------------------------
    def visible(self):
        key = CATEGORIES[self.category][0]
        result = [e for e in self.entries if key == "all" or e["category"] == key]
        if self.search:
            needle = self.search.lower()
            result = [e for e in result if needle in e["name"].lower() or needle in e["id"].lower() or needle in e["description"].lower()]
        return result

    def selected(self):
        entries = self.visible()
        if not entries:
            return None
        self.cursor = max(0, min(self.cursor, len(entries) - 1))
        return entries[self.cursor]

    def plan_for(self, entry):
        """(plan, running mode, error) for the entry under the current handrail setting; dry runs are cached."""
        key = (entry["id"], self.handrail_up)
        if key not in self.plans:
            plan, error = dry_run(self.cli, entry["id"], self.handrail_up)
            running = self.handrail_up
            if plan is None and not self.handrail_up and "--running-mode" in (error or ""):
                plan, error = dry_run(self.cli, entry["id"], True)
                running = True
            self.plans[key] = (plan, running, error)
        return self.plans[key]

    def draw_picker(self):
        s, c = self.screen, self.screen.colors
        height, width = self.window.getmaxyx()
        s.fill(0, c["title"])
        s.put(0, 1, "TREADMILL", c["title"] | curses.A_BOLD)
        settings = "handrail %s · wake %s · manual %s" % ("UP (running)" if self.handrail_up else "DOWN (walking)",
                                                          "wait + ENTER" if self.wake else "now", STRATEGIES[self.strategy])
        if width - len(settings) > 36:
            s.put(0, 11, "· choose a training", c["title"])
        s.put(0, max(12, width - len(settings) - 2), settings, c["title"])
        x = 1
        for index, (_, label) in enumerate(CATEGORIES):
            count = len([e for e in self.entries if CATEGORIES[index][0] in ("all", e["category"])])
            if CATEGORIES[index][0] == "file" and not count:
                continue
            text = " %s %d " % (label, count)
            s.put(2, x, text, c["select"] | curses.A_BOLD if index == self.category else c["dim"])
            x += len(text) + 1
        if self.search is not None:
            s.put(3, 1, "search: " + self.search + "▏", c["accent"])
        entries = self.visible()
        entry = self.selected()
        list_width = min(52, max(36, width // 2 - 2))
        top = 4
        rows = height - top - 2
        first = max(0, self.cursor - rows + 1)
        for row, item in enumerate(entries[first:first + rows]):
            index = first + row
            attribute = c["select"] if index == self.cursor else c["info"]
            duration = clock(item["duration_seconds"]) if item.get("duration_seconds") else "  -  "
            last = item.get("last_run")
            mark = ""
            if last:
                mark = last[0].strftime("%d %b") + (" ✓" if last[1] else " ✗" if last[1] is False else " ·")
            line = " %-*s %6s  %-8s" % (list_width - 19, item["name"][:list_width - 19], duration, mark)
            s.put(top + row, 0, line, attribute | (curses.A_BOLD if index == self.cursor else 0))
            if last and index != self.cursor:
                s.put(top + row, list_width - 7, mark, c["good"] if last[1] else c["warn"])
        if not entries:
            s.put(top, 2, "No trainings here." if self.entries else "`treadmill workout list` returned nothing; is the CLI installed?", c["warn"])
        x0 = list_width + 2
        if entry and width - x0 > 30:
            self.draw_details(entry, top, x0, width - x0 - 1, height - top - 2)
        s.fill(height - 1, c["title"])
        s.put(height - 1, 1, "↑↓ choose  ←→ category  / search  h handrail  w wake  m manual  ENTER start  q quit", c["title"])
        if self.message:
            s.put(height - 2, 1, self.message, c["warn"])

    def draw_details(self, entry, y, x, width, height):
        s, c = self.screen, self.screen.colors
        s.put(y, x, entry["name"], c["accent"] | curses.A_BOLD)
        s.put(y + 1, x, entry["id"] if not entry.get("file") else entry["path"], c["dim"])
        row = y + 3
        for line in textwrap.wrap(entry["description"], width)[:4]:
            s.put(row, x, line, c["info"])
            row += 1
        plan, running, error = self.plan_for(entry)
        row += 1
        if plan is None:
            for line in textwrap.wrap("Cannot run: " + (error or "unknown reason"), width)[:5]:
                s.put(row, x, line, c["danger"])
                row += 1
            return
        steps = plan["workout_steps"]
        total = sum(t["hold_seconds"] for t in steps)
        speeds = [t["speed_code"] / 10 for t in steps]
        levels = [t["incline_code"] for t in steps]
        s.put(row, x, "%s · %d segments · %.1f-%.1f km/h · level %d-%d" % (
            clock(total), len(segments(steps)), min(speeds), max(speeds), min(levels), max(levels)), c["info"])
        distance = sum(t["hold_seconds"] * t["speed_code"] / 36.0 for t in steps)
        s.put(row + 1, x, "≈ %.1f km · ≈ %d m climb (estimated grade)" % (distance / 1000, plan_climb_m(steps)), c["dim"])
        mode = ("RUNNING MODE: raise the handrail" if running else "walking mode: handrail down")
        s.put(row + 2, x, mode, c["warn"] if running else c["good"])
        chart = max(4, min(8, height - (row - y) - 10))
        s.profile(row + 4, x, width - 6, chart, steps)
        row += 6 + chart
        for segment in segments(steps)[:max(0, y + height - row)]:
            first = steps[segment["first"]]
            last = steps[segment["last"]]
            speed = "%.1f" % (first["speed_code"] / 10) if first["speed_code"] == last["speed_code"] else \
                "%.1f→%.1f" % (first["speed_code"] / 10, last["speed_code"] / 10)
            s.put(row, x, "%-24s %6s  %-9s L%d" % (segment["name"][:24], clock(segment["seconds"]), speed, first["incline_code"]),
                  c["level%d" % first["incline_code"]])
            row += 1
            if row >= y + height:
                break

    def picker_key(self, key):
        self.message = ""
        if self.search is not None:
            if key in (27,):
                self.search = None
            elif key in (10, 13, curses.KEY_ENTER):
                self.search = self.search or None
            elif key in (curses.KEY_BACKSPACE, 127, 8):
                self.search = self.search[:-1]
            elif 32 <= key < 127:
                self.search += chr(key)
                self.cursor = 0
            else:
                self.search_navigation(key)
            return True
        if key in (ord("q"), ord("Q"), 3):
            return False
        if key == ord("/"):
            self.search = ""
        elif key == ord("h"):
            self.handrail_up = not self.handrail_up
        elif key == ord("w"):
            self.wake = not self.wake
        elif key == ord("m"):
            self.strategy = (self.strategy + 1) % len(STRATEGIES)
        elif key in (10, 13, curses.KEY_ENTER):
            self.start()
        else:
            self.search_navigation(key)
        return True

    def search_navigation(self, key):
        if key == curses.KEY_UP:
            self.cursor -= 1
        elif key == curses.KEY_DOWN:
            self.cursor += 1
        elif key == curses.KEY_PPAGE:
            self.cursor -= 10
        elif key == curses.KEY_NPAGE:
            self.cursor += 10
        elif key in (curses.KEY_LEFT, curses.KEY_RIGHT, 9):
            step = -1 if key == curses.KEY_LEFT else 1
            while True:
                self.category = (self.category + step) % len(CATEGORIES)
                if CATEGORIES[self.category][0] != "file" or any(e.get("file") for e in self.entries):
                    break
            self.cursor = 0
        self.cursor = max(0, self.cursor)

    # Starting a run ---------------------------------------------------------------------------------------------
    def start(self):
        entry = self.selected()
        if not entry:
            return
        plan, running, error = self.plan_for(entry)
        if plan is None:
            self.message = "Cannot run this training: " + (error or "")
            return
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        stem = Path(entry["id"]).stem if entry.get("file") else entry["id"]
        self.log_path = LOG_DIR / ("%s-%s.jsonl" % (stem, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
        arguments = [self.cli, "workout", "run", entry["id"], "--confirm-motor", "--log", str(self.log_path),
                     "--manual-control", "--manual-strategy", STRATEGIES[self.strategy]]
        if running:
            arguments.append("--running-mode")
        if self.wake:
            arguments += ["--wait-wake", "10", "--wait-wake-confirm"]
        self.state = RunState()
        self.offset_ms = None
        self.briefing, self.stderr = [], StderrParser()
        self.prompt = self.approved = 0
        self.cancel_requested = False
        self.exit_code = None
        self.alerted = set()
        self.run_entry, self.run_plan, self.run_running = entry, plan, running
        self.export_message = ""
        self.child = Child(arguments)
        self.mode = "briefing"

    def cli_now(self):
        if self.offset_ms is None:
            return self.state.last_ms if self.state else 0
        return time.monotonic() * 1000 - self.offset_ms

    def pump(self):
        if not self.child:
            return
        while True:
            try:
                kind, value = self.child.events.get_nowait()
            except queue.Empty:
                return
            if kind == "record":
                if "monotonic_ms" in value:
                    offset = time.monotonic() * 1000 - value["monotonic_ms"]
                    self.offset_ms = offset if self.offset_ms is None else min(self.offset_ms, offset)
                self.state.feed(value)
                if value.get("event") == "raw_status" and value.get("status_code") == 6 and value.get("disabled_reason_code") == 1:
                    self.alert(flash=True)
                if value.get("event") in ("fault", "console_pause"):
                    self.alert()
            elif kind == "stderr":
                self.stderr_text(value)
            elif kind == "stdout":
                self.state.say("info", value)
            elif kind == "exit":
                self.exit_code = value
                if self.mode in ("briefing", "run"):
                    self.mode = "summary"

    def stderr_text(self, text):
        if "Type MOTOR" in text:  # a CLI from before 2026-10-28
            self.briefing.append("THIS CLI IS TOO OLD for the TUI (it asks for a typed MOTOR): rebuild and reinstall it (README, Build). q cancels.")
        for line in self.stderr.feed(text):
            if self.mode == "briefing" and self.approved < 2:
                self.briefing.append(line)
            else:
                self.state.say("info", line.strip())
        if self.stderr.prompts > self.approved:
            self.prompt = self.approved + 1
            if self.cancel_requested:
                self.child.stop()

    def alert(self, flash=False):
        if self.beeps:
            curses.beep()
        if flash:
            self.flash_until = time.monotonic() + 6

    # Briefing (the CLI's approval) ------------------------------------------------------------------------------
    def draw_briefing(self):
        s, c = self.screen, self.screen.colors
        height, width = self.window.getmaxyx()
        s.fill(0, c["title"])
        name = self.run_plan.get("workout_name") or self.run_entry["name"]
        s.put(0, 1, "TREADMILL · approve " + name, c["title"] | curses.A_BOLD)
        steps = self.run_plan["workout_steps"]
        s.put(2, 2, "%s · %s · %.1f-%.1f km/h · level %d-%d" % (
            name, clock(sum(t["hold_seconds"] for t in steps)), min(t["speed_code"] for t in steps) / 10,
            max(t["speed_code"] for t in steps) / 10, min(t["incline_code"] for t in steps), max(t["incline_code"] for t in steps)),
              c["accent"] | curses.A_BOLD)
        s.put(3, 2, "RUNNING MODE: handrail up" if self.run_running else "walking mode: handrail down",
              c["warn"] if self.run_running else c["good"])
        s.put(3, 32, "manual %s · %s" % (STRATEGIES[self.strategy], "waits for the wake, then ENTER starts the belt" if self.wake
                                          else "starts as soon as the treadmill answers"), c["dim"])
        row = 5
        for line in self.briefing:
            for part in textwrap.wrap(line, width - 4) or [""]:
                if row >= height - 6:
                    break
                attribute = c["warn"] if part.isupper() or part.split(":")[0].isupper() else c["info"]
                s.put(row, 2, part, attribute)
                row += 1
        row = max(row + 1, height - 5)
        if self.prompt == 1:
            s.put(row, 2, " Press ENTER to approve this run ", c["badge_warn"] | curses.A_BOLD)
            s.put(row + 1, 2, "Anything else cancels. Keys pressed before this prompt appeared were discarded.", c["dim"])
        elif self.prompt == 2:
            s.put(row, 2, " Press ENTER again to confirm ", c["badge_danger"] | curses.A_BOLD)
            s.put(row + 1, 2, "Second confirmation: belt clear, safety key attached, you at the controls.", c["dim"])
        else:
            s.put(row, 2, "Preparing the run…", c["dim"])
        s.fill(height - 1, c["title"])
        s.put(height - 1, 1, "ENTER approve   Esc/q cancel", c["title"])

    def briefing_key(self, key):
        if key in (10, 13, curses.KEY_ENTER) and self.prompt in (1, 2):
            self.prompt, self.approved = 0, self.approved + 1
            self.child.send("\n")
            if self.approved == 2:
                self.mode = "run"
        elif key in (27, ord("q"), ord("Q"), 3):
            self.cancel_requested = True
            self.child.stop()  # before a prompt the CLI discards it, so it is sent again when the prompt appears
        return True

    # Run screen -------------------------------------------------------------------------------------------------
    def phase_badge(self, now):
        st = self.state
        phases = {
            "connecting": ("CONNECTING", "badge_info"), "preflight": ("PREFLIGHT", "badge_info"),
            "asleep": ("WAKE THE CONSOLE", "badge_warn"), "awake": ("PRESS ENTER TO START", "badge_good"),
            "grace": ("STARTING IN %d s" % max(0, ((st.grace_until or now) - now) // 1000 + 1), "badge_warn"),
            "starting": ("STARTING", "badge_info"), "countdown": ("COUNTDOWN", "badge_warn"),
            "running": ("RUNNING", "badge_good"), "paused": ("PAUSED", "badge_warn"), "resuming": ("RESUMING", "badge_info"),
            "stopping": ("STOPPING", "badge_danger"), "finished": ("FINISHED", "badge_info"),
        }
        return phases.get(st.phase, (st.phase.upper(), "badge_info"))

    def draw_run(self):
        s, c, st = self.screen, self.screen.colors, self.state
        height, width = self.window.getmaxyx()
        now = self.cli_now()
        s.fill(0, c["title"])
        s.put(0, 1, "▶ " + (st.name or "…"), c["title"] | curses.A_BOLD)
        badge, color = self.phase_badge(now)
        right = "%s · %s" % ("running mode" if st.profile == "running" else "walking mode", time.strftime("%H:%M"))
        s.put(0, width - len(right) - 2, right, c["title"])
        s.put(0, max(len(st.name) + 5, width // 2 - len(badge) // 2), " " + badge + " ", c[color] | curses.A_BOLD)
        # Link health: the helper's watchdog stops the belt when status is older than 1.5 s.
        if st.status_at is not None and st.phase not in ("finished",):
            age = max(0, now - st.status_at) / 1000
            link = c["good"] if age < 1.0 else c["warn"] if age < 1.5 else c["danger"]
            s.put(1, width - 18, "● link %4.1f s" % age, link | curses.A_BOLD)
        if width < 80 or height < 24:
            s.put(2, 1, "Enlarge the terminal to at least 80x24.", c["warn"])
            s.put(3, 1, "%.1f km/h  level %d  %s" % (st.speed / 10, st.incline, badge), c["big"])
            s.put(height - 1, 0, " q STOP ", c["badge_danger"])
            return
        card = (width - 2) // 4
        moving = st.status == 3
        metres, source = st.distance()
        cards = [
            ("SPEED  km/h", "%.1f" % (st.speed / 10 if moving or st.phase in ("running", "resuming") else 0),
             ("target %.1f" % (st.target_speed / 10), ("%s /km" % clock(3600 / (st.speed / 10))) if moving and st.speed else "-- /km"),
             "accent"),
            ("INCLINE  level", "%d" % st.incline, ("≈ %.1f %% grade" % (st.incline * GRADE_PER_LEVEL), "target %d" % st.target_incline),
             "level%d" % max(0, min(8, st.incline))),
            ("DISTANCE  km", "%.2f" % (metres / 1000), ("≈ %d m climb" % st.climb_m, "treadmill counter" if source == "treadmill" else "estimated"),
             "good"),
            ("TIME", "%d:%02d" % divmod(int(st.plan_elapsed_ms(now) / 1000), 60),
             ("of %s" % clock(st.plan_total_ms() / 1000), "%s left" % clock((st.plan_total_ms() - st.plan_elapsed_ms(now)) / 1000)),
             "big"),
        ]
        for index, (title, value, detail, color) in enumerate(cards):
            x = 1 + index * card
            s.box(2, x, 9, card - 1, title, c["dim"])
            text_width = len(value) * 4 - 1
            if text_width <= card - 3:
                s.big(3, x + max(1, (card - 1 - text_width) // 2), value, c[color] | curses.A_BOLD)
            else:  # too narrow for the big digits
                s.put(5, x + max(1, (card - 1 - len(value)) // 2), value, c[color] | curses.A_BOLD)
            if len(detail[0]) + len(detail[1]) + 3 <= card - 4:
                s.put(9, x + 2, detail[0] + " · " + detail[1], c["dim"], card - 4)
            else:
                s.put(8, x + 2, detail[0], c["dim"], card - 4)
                s.put(9, x + 2, detail[1], c["dim"], card - 4)
        row = 12
        row = self.draw_banner(row, now, width)
        row = self.draw_plan_lines(row, now, width)
        chart_height = max(3, min(7, height - row - 9))
        if st.steps:
            s.profile(row, 2, width - 8, chart_height, st.steps, st.plan_elapsed_ms(now) / 1000)
            row += chart_height + 2
        for at, level, text in st.messages[-max(0, height - row - 1):]:
            stamp = clock((at - (st.running_at or 0)) / 1000) if at is not None and st.running_at is not None and at >= st.running_at else "     "
            s.put(row, 2, "%6s  %s" % (stamp, text), c[level] if level != "info" else c["dim"], width - 4)
            row += 1
        s.fill(height - 1, c["title"])
        keys = [(" q STOP ", "badge_danger"), (" space pause ", "title"), (" ↑↓ speed ", "title"), (" ←→ incline ", "title"),
                (" r plan ", "title"), (" b beeps %s " % ("on" if self.beeps else "off"), "title")]
        if st.phase == "awake":
            keys.insert(1, (" ENTER start belt ", "badge_good"))
        x = 0
        for text, color in keys:
            s.put(height - 1, x, text, c[color] | curses.A_BOLD)
            x += len(text) + 1

    def draw_banner(self, row, now, width):
        s, c, st = self.screen, self.screen.colors, self.state
        text, color = None, None
        if st.key_removed:
            utc = st.key_removed[1]
            local = ""
            try:
                local = datetime.datetime.fromisoformat(utc.replace("Z", "+00:00")).astimezone().strftime("%H:%M:%S")
            except (AttributeError, ValueError):
                pass
            into = ", %s into the run" % clock((st.key_removed[0] - st.running_at) / 1000) if st.running_at is not None else ""
            text = "⚠ SAFETY KEY REMOVED at %s%s. Belt stopping; reattach the key for a new run." % (local, into)
            color = "badge_danger"
            if time.monotonic() < self.flash_until and int(time.monotonic() * 3) % 2:
                color = "badge_warn"
        elif st.phase == "asleep":
            text, color = "The treadmill is asleep. Wake it at the console; nothing is sent to the motor meanwhile.", "badge_warn"
        elif st.phase == "awake":
            text, color = "The treadmill is awake. Step on, then press ENTER to start the belt (q cancels).", "badge_good"
        elif st.phase == "grace":
            text, color = "The belt starts by itself in a few seconds; q cancels.", "badge_warn"
        elif st.phase == "paused":
            left = st.pause_allowance_left_s(now)
            text = "PAUSED %s. Press Play on the console to continue (%s of pause allowance left)." % (
                clock((now - (st.pause_started or now)) / 1000), clock(left))
            color = "badge_warn" if left > 120 else "badge_danger"
        elif st.phase == "resuming":
            text, color = "Returning to the plan: %.1f km/h, level %d…" % (st.target_speed / 10, st.target_incline), "badge_info"
        elif st.phase == "stopping":
            text, color = "STOPPING: %s" % (st.stop_reason or ""), "badge_danger"
        elif st.faults:
            text, color = "Fault: " + st.faults[-1], "badge_danger"
        if text:
            s.put(row, 1, (" " + text + " ").ljust(width - 2), c[color] | curses.A_BOLD, width - 2)
            return row + 2
        return row + 1

    def draw_plan_lines(self, row, now, width):
        s, c, st = self.screen, self.screen.colors, self.state
        if not st.segments:
            return row
        index = st.segment_index()
        segment = st.segments[index]
        left = st.segment_left_ms(now) / 1000
        step = st.steps[st.stage]
        reaching = st.stage not in st.step_start and st.phase == "running"
        label = segment["name"] + (" %d/%d" % (segment["number"], segment["of"]) if segment["of"] > 1 else "")
        s.put(row, 2, "NOW ", c["accent"] | curses.A_BOLD)
        s.put(row, 7, "%-26s %.1f km/h · level %d" % (label[:26], step["speed_code"] / 10, step["incline_code"]), c["info"] | curses.A_BOLD)
        if st.phase == "running" and (st.target_speed, st.target_incline) != (step["speed_code"], step["incline_code"]):
            s.put(row, 57, "now %.1f · L%d (manual)" % (st.target_speed / 10, st.target_incline), c["warn"] | curses.A_BOLD, width - 82)
        status = "reaching the target…" if reaching else "%s left" % clock(left)
        s.put(row, width - 24, status, c["dim"])
        fraction = 1 - left / segment["seconds"] if segment["seconds"] else 0
        s.bar(row + 1, 7, width - 33, fraction, c["accent"], c["dim"])
        if segment["first"] != segment["last"]:
            ramp = "ramp %d/%d, next step %.1f km/h · level %d in %s" % (
                st.stage - segment["first"] + 1, segment["last"] - segment["first"] + 1,
                st.steps[min(st.stage + 1, segment["last"])]["speed_code"] / 10,
                st.steps[min(st.stage + 1, segment["last"])]["incline_code"], clock(st.step_left_ms(now) / 1000))
            s.put(row + 2, 7, ramp if st.stage < segment["last"] else "last ramp step", c["dim"])
        row += 3
        upcoming = st.segments[index + 1:index + 3]
        if not upcoming:
            s.put(row, 2, "NEXT", c["next"] | curses.A_BOLD)
            s.put(row, 7, "Finish in %s: the CLI stops the belt and waits until it reports stopped." % clock(left), c["dim"])
            return row + 2
        before = step
        for number, nxt in enumerate(upcoming):
            first = st.steps[nxt["first"]]
            speed_change = (first["speed_code"] - before["speed_code"]) / 10
            level_change = first["incline_code"] - before["incline_code"]
            before = st.steps[nxt["last"]]
            change = []
            if speed_change:
                change.append("%+.1f km/h" % speed_change)
            if level_change:
                change.append("%+d level%s" % (level_change, "s" if abs(level_change) > 1 else ""))
            label = nxt["name"] + (" %d/%d" % (nxt["number"], nxt["of"]) if nxt["of"] > 1 else "")
            when = left + sum(sg["seconds"] for sg in upcoming[:number])
            soon = number == 0 and when <= 10 and st.phase == "running"
            attribute = c["next"] | curses.A_BOLD if number == 0 else c["dim"]
            if soon:
                attribute = c["badge_warn"] | curses.A_BOLD
                key = ("heads-up", nxt["first"])
                if key not in self.alerted:
                    self.alerted.add(key)
                    self.alert()
            s.put(row, 2, "NEXT" if number == 0 else "THEN", c["next"] | curses.A_BOLD)
            name_width = 24 if width >= 100 else 14
            s.put(row, 7, " in %-6s ▸ %-*s %.1f km/h · L%d  %s " % (
                clock(when), name_width, label[:name_width], first["speed_code"] / 10, first["incline_code"], ("(" + ", ".join(change) + ")") if change else ""),
                  attribute, width - 9)
            row += 1
        return row + 1

    def run_key(self, key):
        st = self.state
        if key in (ord("q"), ord("Q"), ord("s"), ord("S"), 3):  # not Esc: a split arrow-key sequence must not stop the run
            self.child.stop()
            st.say("warn", "Stop requested from the keyboard.")
        elif key in (10, 13, curses.KEY_ENTER) and st.phase == "awake":
            self.child.send("\n")
        elif key in (ord(" "), ord("p"), ord("P")):
            if st.pause_supported is False:
                st.say("warn", "Pause from here is not in the CLI yet: press Pause/Play on the console.")
            else:
                cmd = "resume" if st.phase == "paused" else "pause"
                st.pending[self.child.command(cmd)] = cmd
        elif key in (curses.KEY_UP, ord("+"), ord("=")):
            self.child.command("speed_up")
        elif key in (curses.KEY_DOWN, ord("-")):
            self.child.command("speed_down")
        elif key == curses.KEY_PPAGE:
            for _ in range(5):
                self.child.command("speed_up")
        elif key == curses.KEY_NPAGE:
            for _ in range(5):
                self.child.command("speed_down")
        elif key in (curses.KEY_RIGHT, ord("]")):
            self.child.command("incline_up")
        elif key in (curses.KEY_LEFT, ord("[")):
            self.child.command("incline_down")
        elif key in (ord("r"), ord("R")):
            self.child.command("reset")
        elif key in (ord("b"), ord("B")):
            self.beeps = not self.beeps
        return True

    # Summary ----------------------------------------------------------------------------------------------------
    def draw_summary(self):
        s, c, st = self.screen, self.screen.colors, self.state
        height, width = self.window.getmaxyx()
        s.fill(0, c["title"])
        s.put(0, 1, "TREADMILL · run summary", c["title"] | curses.A_BOLD)
        finished = st.result is not None
        if st.completed() and not st.faults and self.exit_code == 0:
            headline, color = " ✓ COMPLETED " + (st.name or ""), "badge_good"
        elif st.running_at is None:
            headline, color = " The run did not start ", "badge_warn"
        else:
            headline, color = " ✗ ENDED EARLY " + (st.name or ""), "badge_danger"
        s.put(2, 2, headline + " ", c[color] | curses.A_BOLD)
        metres, source = st.distance()
        moving = st.moving_ms(st.last_ms) / 1000
        rows = [
            ("Moving time", clock(moving)),
            ("Plan progress", "%s of %s" % (clock(st.plan_elapsed_ms(st.last_ms) / 1000), clock(st.plan_total_ms() / 1000))),
            ("Distance", "%.2f km (%s)" % (metres / 1000, "reported by the treadmill" if source == "treadmill" else "estimated from speed")),
            ("Climb", "≈ %d m (estimated grade per level)" % st.climb_m),
            ("Average speed", "%.1f km/h" % (metres / moving * 3.6) if moving > 0 else "-"),
            ("Top speed", "%.1f km/h" % (st.max_speed / 10)),
            ("Console pauses", "%d (%s)" % (st.pauses, clock(st.idle_ms / 1000))),
            ("Stop reason", st.stop_reason or "-"),
        ]
        if st.key_removed:
            rows.append(("Safety key", "removed %s into the run" % clock((st.key_removed[0] - (st.running_at or st.key_removed[0])) / 1000)))
        for fault in st.faults[-3:]:
            rows.append(("Fault", fault))
        if not finished:
            rows.append(("CLI", "exited with code %s before its final record" % self.exit_code))
        row = 4
        for label, value in rows:
            s.put(row, 4, "%-16s" % label, c["dim"])
            s.put(row, 21, value, c["info"] | curses.A_BOLD, width - 23)
            row += 1
        row += 1
        if self.log_path:
            s.put(row, 4, "Log", c["dim"])
            s.put(row, 21, str(self.log_path), c["accent"], width - 23)
            s.put(row + 1, 21, "python3 %s %s" % (HERE / "summarize_run.py", self.log_path), c["dim"], width - 23)
            s.put(row + 2, 21, self.export_message or "press f to export a Garmin FIT activity next to the log", c["dim"], width - 23)
            row += 4
        if st.steps:
            s.profile(row, 4, width - 12, max(3, min(6, height - row - 4)), st.steps, st.plan_elapsed_ms(st.last_ms) / 1000)
        s.fill(height - 1, c["title"])
        s.put(height - 1, 1, "ENTER back to trainings   f FIT export   q quit", c["title"])

    def summary_key(self, key):
        if key in (ord("q"), ord("Q"), 3):
            return False
        if key in (ord("f"), ord("F")) and self.log_path and self.child and self.child.process.poll() is not None:
            self.export_message = export_activity(self.log_path, self.run_entry)
        elif key in (10, 13, curses.KEY_ENTER, 27):
            self.finish_child()
            self.entries = load_catalog(self.cli, self.extra_files)
            self.mode = "picker"
        return True

    def finish_child(self):
        if self.child:
            self.child.close()
            self.child = None

    # Main loop --------------------------------------------------------------------------------------------------
    def follow_resize(self):
        """Pick up a terminal resize even when no KEY_RESIZE arrives (seen under tmux)."""
        try:
            size = os.get_terminal_size(sys.__stdout__.fileno())
        except OSError:
            return
        if (size.lines, size.columns) != self.window.getmaxyx():
            curses.resizeterm(size.lines, size.columns)

    def loop(self):
        curses.curs_set(0)
        self.window.keypad(True)
        self.window.timeout(100)
        while True:
            self.pump()
            self.follow_resize()
            self.window.erase()
            {"picker": self.draw_picker, "briefing": self.draw_briefing, "run": self.draw_run,
             "summary": self.draw_summary}[self.mode]()
            self.window.refresh()
            key = self.window.getch()
            if key == -1 or key == curses.KEY_RESIZE:
                continue
            handler = {"picker": self.picker_key, "briefing": self.briefing_key, "run": self.run_key,
                       "summary": self.summary_key}[self.mode]
            if not handler(key):
                return


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else argv
    if any(a in ("-h", "--help") for a in arguments):
        print(__doc__.strip())
        return 0
    locale.setlocale(locale.LC_ALL, "")
    os.environ.setdefault("ESCDELAY", "25")
    holder = {}

    def run(window):
        curses.raw()  # Ctrl-C arrives as a key (stop), not as a signal
        holder["app"] = App(window, arguments)
        holder["app"].loop()

    try:
        curses.wrapper(run)
    finally:
        app = holder.get("app")
        if app and app.child and app.child.process.poll() is None:
            print("Stopping the run: waiting for the CLI to stop the belt…", flush=True)
        if app:
            app.finish_child()
            if app.log_path and app.log_path.exists():
                print("Log: %s" % app.log_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
