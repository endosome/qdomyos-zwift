"""Bounded motor trial with a fake local BLE peer; no hardware I/O."""
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ADDRESS = 'AA:BB:CC:DD:EE:FF'
PROMPTS = (b'Press ENTER to approve', b'Press ENTER again')


def approve(master, stderr, answers=(b'\n', b'\n'), timeout=30):
    """Answer the CLI's two ENTER prompts, each once it is printed: the CLI discards input typed before a prompt.
    Stops after an answer other than ENTER (the CLI cancels). stderr is a PIPE or a file; returns what was read from a PIPE."""
    consumed, closed = b'', False
    def text():
        nonlocal consumed, closed
        if stderr.seekable():
            time.sleep(0.01)
            stderr.seek(0)
            return stderr.read()
        if select.select([stderr], [], [], 0.01)[0]:
            chunk = os.read(stderr.fileno(), 65536)
            consumed, closed = consumed + chunk, not chunk
        return consumed
    deadline = time.monotonic() + timeout
    for prompt, answer in zip(PROMPTS, answers):
        while prompt not in text():
            if closed:  # the CLI refused before asking
                return consumed
            if time.monotonic() > deadline:
                raise AssertionError('The CLI never printed %r; stderr: %s' % (prompt, text()[-1500:]))
        os.write(master, answer)
        if answer != b'\n':
            break
    return consumed


class MotorTrialTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.log = self.directory / 'trial.jsonl'
        self.calls = self.directory / 'frames.json'
        self.environment = dict(os.environ, PATH=str(self.directory) + os.pathsep + os.environ['PATH'],
                                MOTOR_CALLS=str(self.calls), MOTOR_MODE='normal', TREADMILL_STATE_DIR=str(self.directory / 'state'))
        self.executable('wslpath', 'import sys\nprint(sys.argv[-1])\n')
        helper = self.executable('fake-powershell', '''import json, os, sys, time
from pathlib import Path
mode = os.environ['MOTOR_MODE']
def argument(windows, linux):
    for name in (windows, linux):
        if name in sys.argv:
            return sys.argv[sys.argv.index(name) + 1]
trial = argument('-Trial', '--trial')
expected_name = argument('-ExpectedName', '--expected-name')
running_profile = argument('-Profile', '--profile') == 'running'
Path(os.environ['MOTOR_CALLS'] + '.argv').write_text(json.dumps(sys.argv[1:]))
if mode == 'batch-fault':
    mode = 'bounds' if trial == 'speed-60' else 'normal'
observe_ftms = '-ObserveFtms' in sys.argv or '--observe-ftms' in sys.argv
calls = []
moving = False
ready = False
ready_at = 0
stopped = False
target_count = 0
speed, incline, paused = 10, 0, False
normal_at = 0
polls = 0
console_pause = [float(v) for v in os.environ['MOTOR_CONSOLE_PAUSE'].split(',')] if os.environ.get('MOTOR_CONSOLE_PAUSE') else None
pause_begin = None
def console_phase():
    # None (no pause), 'stopping', 'normal', 'countdown' or 'resumed'.
    if pause_begin is None:
        return None
    elapsed = time.monotonic() - pause_begin
    if elapsed < 0:
        return None
    return 'stopping' if elapsed < 1 else 'normal' if elapsed < console_pause[2] else 'countdown' if elapsed < console_pause[2] + 1 else 'resumed'
def send(line):
    print(line, flush=True)
def rx(payload):
    checksum = 0
    for byte in payload:
        checksum ^= byte
    send('RX\\t' + (bytes([2]) + payload + bytes([checksum,3])).hex())
send('IDENTITY\\tFITSHOW\\tFS-BT-C1\\t1.3.8')
send('READY\\t' + expected_name)
if observe_ftms:
    send('FTMS\\t00002acd-0000-1000-8000-00805f9b34fb\\t00000a00')
for line in sys.stdin:
    if line.strip() == 'QUIT':
        break
    kind, ident, frame = line.strip().split('\\t')
    calls.append(frame)
    Path(os.environ['MOTOR_CALLS']).write_text(json.dumps(calls))
    payload = bytes.fromhex(frame)[1:-2]
    send('TX\\t' + ident + '\\t' + frame + '\\tSuccess')
    if payload == bytes.fromhex('5002'):
        # 10-120 with unit tag 0 is the console's report with the handrail raised; 'unknown-tag' is the older unexplained 0x20 variant.
        rx(bytes.fromhex('5002780a20') if mode == 'unknown-tag' else bytes.fromhex('5002780a00') if (mode == 'bounds' or running_profile) else bytes.fromhex('50023c0a00'))
    elif payload == bytes.fromhex('5003'):
        rx(bytes.fromhex('5003080002'))
    elif payload[0] == 0x51:
        if os.environ.get('MOTOR_WAKE_AFTER') is not None and not moving and not stopped:
            polls += 1
            asleep = polls <= int(os.environ['MOTOR_WAKE_AFTER'])
            if os.environ.get('MOTOR_RESLEEP'):
                begin, length = map(int, os.environ['MOTOR_RESLEEP'].split(','))
                asleep = asleep or begin <= polls < begin + length
            if asleep:
                rx(bytes.fromhex('510602'))
                continue
            if os.environ.get('MOTOR_WAKE_KEY'):
                rx(bytes.fromhex('510601'))
                continue
        if not moving and not ready and not stopped and mode in ('sleep', 'key', 'unknown-disabled', 'sleep-stays'):
            rx(bytes.fromhex('510602') if mode in ('sleep', 'sleep-stays') else
               (bytes.fromhex('510601') if mode == 'key' else bytes.fromhex('5106')))
            continue
        if ready and mode == 'ready-reverted' and time.monotonic() - ready_at > 1:
            ready = False
        phase = console_phase() if not stopped else None
        if phase in ('stopping', 'normal', 'countdown'):
            moving = False
        elif phase == 'resumed' and not globals().get('resumed'):
            globals()['resumed'] = True
            moving, speed, incline, odometer = True, 10, 0, 0  # Play restarts the belt at 1.0 km/h and the distance counter at zero
        if phase in ('stopping', 'normal', 'countdown') and observe_ftms:
            send('FTMS\t00002acd-0000-1000-8000-00805f9b34fb\t' + ('8c05' + '0000' + globals().get('odometer', 0).to_bytes(3, 'little').hex() + '00' * 12))
        if phase == 'stopping':
            rx(bytes([0x51, 4, 5, incline]) + bytes(9))
            continue
        if phase == 'normal':
            rx(bytes.fromhex('5100'))
            continue
        if phase == 'countdown':
            rx(bytes.fromhex('510201'))
            continue
        if observe_ftms and os.environ.get('MOTOR_FTMS', 'ok') != 'silent':
            shown = (speed if moving else 0) * (0.5 if os.environ.get('MOTOR_FTMS') == 'mismatch' else 1)
            odometer = globals().get('odometer', 0) + (3 if moving else 0)  # the fake treadmill's own distance counter
            globals()['odometer'] = odometer
            send('FTMS\t00002acd-0000-1000-8000-00805f9b34fb\t' + ('8c05' + int(shown * 10).to_bytes(2, 'little').hex() + odometer.to_bytes(3, 'little').hex() + '00' * 12))
        rx(bytes([0x51, 4, 1, 0]) + bytes(9) if time.monotonic() < normal_at else
           (bytes([0x51, 10, 0, incline]) + bytes(9) if paused else
           (bytes([0x51, 3, speed, incline]) + bytes(9) if moving else
            (bytes.fromhex('5109') if ready else bytes.fromhex('5100'))))
           )
    elif payload[:2] == bytes.fromhex('5301'):
        if trial in ('ready-idle', 'ready-wake'):
            ready = mode != 'sleep-stays'
            ready_at = time.monotonic()
            moving = mode == 'ready-countdown'
            if moving:
                rx(bytes.fromhex('510203'))
        if mode == 'disconnect':
            sys.exit(1)
        if mode != 'no-ack':
            rx(bytes.fromhex('5301'))
    elif payload[:2] == bytes.fromhex('5302'):
        if console_phase() in ('stopping', 'normal', 'countdown'):
            Path(os.environ['MOTOR_CALLS'] + '.paused').write_text(frame)  # the host must never send a target to a paused console
        moving = True
        speed, incline = payload[2], payload[3]
        target_count += 1
        if console_pause and pause_begin is None and target_count == int(console_pause[0]):
            pause_begin = time.monotonic() + console_pause[1]
        rx(bytes.fromhex('5302'))
        if mode == 'safety' or (mode == 'workout-fault' and target_count == 2):
            rx(bytes.fromhex('510602'))
    elif payload == bytes.fromhex('5303'):
        stopped = True
        moving, paused, ready = False, False, False
        if os.environ.get('MOTOR_STOP_DELAY'):
            normal_at = time.monotonic() + float(os.environ['MOTOR_STOP_DELAY'])
        if mode == 'delayed-stop':
            normal_at = time.monotonic() + 4.4
        rx(bytes.fromhex('5303'))
    elif payload == bytes.fromhex('5306'):
        moving, paused = False, True
        rx(bytes.fromhex('5306'))
send(json.dumps(dict(command='control-test', backend=os.environ.get('MOTOR_BACKEND', 'windows-winrt'), status='complete',
                     notification_cleanup_status='Success', motor_execution_enabled=True,
                     ftms_observation_status='complete' if observe_ftms else 'not requested')))
''')
        self.environment['TREADMILL_POWERSHELL'] = str(helper)
        self.arguments = [BINARY, 'control-test', '--backend', 'windows', '--device', ADDRESS,
                          '--expected-name', 'FS-TEST']

    def tearDown(self):
        self.temporary.cleanup()

    def executable(self, name, source):
        path = self.directory / name
        path.write_text('#!' + sys.executable + '\n' + source)
        path.chmod(0o755)
        return path

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def trial(self, mode='normal', confirmation=(b'\n', b'\n'), terminate=False, trial='start-stop'):
        self.environment['MOTOR_MODE'] = mode
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([*self.arguments, '--trial', trial, '--confirm-motor', '--log', str(self.log)],
                                   stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            early = approve(master, process.stderr, confirmation)
            if terminate:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if self.log.exists() and '"event":"start_requested"' in self.log.read_text():
                        process.send_signal(signal.SIGTERM)
                        break
                    time.sleep(0.01)
                else:
                    self.fail('Motor request never reached fake peer')
            output, errors = process.communicate(timeout=180)
            self.assertEqual(termios.tcgetattr(slave), before)
            return process.returncode, output, early + errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_dry_run_and_refusals_do_not_start_helper(self):
        result = subprocess.run([*self.arguments, '--dry-run'], capture_output=True, text=True, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertEqual(plan['frames'], ['02530100000000000000005203', '0253020a005b03', '0253035003'])
        self.assertFalse(plan['motor_execution_enabled'])
        self.assertFalse(self.calls.exists())
        result = subprocess.run(self.arguments, capture_output=True, text=True, env=self.environment)
        self.assertEqual(result.returncode, 2)
        result = subprocess.run([*self.arguments, '--confirm-motor', '--log', str(self.log)],
                                stdin=subprocess.DEVNULL, capture_output=True, env=self.environment)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.calls.exists())
        code, _, _ = self.trial(confirmation=(b'NO\n',))
        self.assertEqual(code, 2)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.calls.exists())

    def test_bounded_run_reuses_queue_and_labels_physical_stop_unknown(self):
        code, _, errors = self.trial()
        self.assertEqual(code, 0, errors)
        frames = json.loads(self.calls.read_text())
        motor = [value for value in frames if value.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03', '0253035003'])
        events = self.events()
        running = next(row for row in events if row['event'] == 'reported_running')
        stopped = next(row for row in events if row['event'] == 'stop_requested')
        self.assertGreaterEqual(stopped['monotonic_ms'] - running['monotonic_ms'], 5000)
        self.assertTrue(events[-1]['sequence_observed'])
        self.assertFalse(events[-1]['physical_stop_verified'])

    def workout(self, mode='normal', backend='windows'):
        self.environment['MOTOR_MODE'] = mode
        workout = self.directory / 'short-w1.xml'
        workout.write_text('''<rows version="1" id="short-w1" name="Short W1 schedule" speedunit="km/h" inclineunit="percent" device="treadmill">
  <row name="Baseline" duration="00:00:01" forcespeed="1" speed="1" inclination="0" />
  <row name="Speed step" duration="00:00:01" forcespeed="1" speed="2" inclination="0" />
  <row name="Incline step" duration="00:00:01" forcespeed="1" speed="2" inclination="1.375" />
</rows>''')
        arguments = [BINARY, 'workout', 'run', str(workout), '--backend', backend,
                     '--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32',
                     '--confirm-motor', '--log', str(self.log)]
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen(arguments, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=self.environment)
        try:
            early = approve(master, process.stderr)
            output, errors = process.communicate(timeout=45)
            self.assertEqual(termios.tcgetattr(slave), before)
            return process.returncode, output, early + errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_windows_workout_runs_ordered_targets_and_stops(self):
        code, _, errors = self.workout()
        self.assertEqual(code, 0, errors)
        motor = [frame for frame in json.loads(self.calls.read_text()) if frame.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03',
                                 '02530214004503', '02530214014403', '0253035003'])
        rows = self.events()
        requested = [row for row in rows if row['event'] == 'target_requested']
        self.assertEqual([(row['stage'], row['speed_code'], row['incline_code']) for row in requested],
                         [(1, 20, 0), (2, 20, 1)])
        self.assertTrue(rows[-1]['sequence_observed'])
        self.assertFalse(rows[-1]['physical_stop_verified'])

    def test_windows_workout_safety_fault_stops_before_later_targets(self):
        code, _, _ = self.workout(mode='workout-fault')
        self.assertEqual(code, 1)
        motor = [frame for frame in json.loads(self.calls.read_text()) if frame.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03',
                                 '02530214004503', '0253035003'])
        rows = self.events()
        self.assertTrue(any(row['event'] == 'fault' for row in rows))
        self.assertFalse(any(row.get('stage') == 2 for row in rows if row['event'] == 'target_requested'))
        self.assertFalse(rows[-1]['sequence_observed'])
        self.assertFalse(rows[-1]['physical_stop_verified'])

    def test_changed_limits_refuse_before_motor(self):
        code, _, _ = self.trial(mode='bounds')
        self.assertEqual(code, 1)
        self.assertFalse(any(frame.startswith('0253') for frame in json.loads(self.calls.read_text())))

    def test_lost_start_ack_is_not_retried_and_stop_overtakes_target(self):
        code, _, _ = self.trial(mode='no-ack')
        self.assertEqual(code, 1)
        motor = [value for value in json.loads(self.calls.read_text()) if value.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253035003'])

    def test_safety_fault_and_signal_request_stop(self):
        for mode, terminate in [('safety', False), ('normal', True)]:
            self.log.unlink(missing_ok=True)
            self.calls.unlink(missing_ok=True)
            code, _, _ = self.trial(mode=mode, terminate=terminate)
            self.assertEqual(code, 1)
            frames = json.loads(self.calls.read_text())
            stop = frames.index('0253035003')
            self.assertFalse(any(frame.startswith('0253') for frame in frames[stop + 1:]))
            self.assertFalse(self.events()[-1]['physical_stop_verified'])

    def test_disconnect_never_claims_delivered_stop(self):
        code, _, _ = self.trial(mode='disconnect')
        self.assertEqual(code, 1)
        self.assertFalse(self.events()[-1]['stop_write_succeeded'])
        self.assertFalse(self.events()[-1]['sequence_observed'])

    def test_all_prepared_plans_are_dry_and_bounded(self):
        result = subprocess.run([BINARY, 'control-test', '--backend', 'windows', '--list-trials'],
                                capture_output=True, text=True, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        catalog = json.loads(result.stdout)
        self.assertEqual(len(catalog), 23)
        self.assertEqual({r['id'] for r in catalog}, {'start-stop', 'pause', 'incline-timing-4', 'incline-timing-8', 'incline-measure-4', 'incline-measure-8', 'ready-idle', 'ready-wake'} |
                         {'speed-' + str(code) for code in (12, 15, 20, 30, 40, 50, 60)} |
                         {'incline-' + str(code) for code in range(1, 9)})
        for trial in catalog:
            result = subprocess.run([*self.arguments, '--trial', trial['id'], '--dry-run'],
                                    capture_output=True, text=True, env=self.environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            plan = json.loads(result.stdout)
            if trial['id'].startswith('incline-measure-'):
                self.assertEqual(plan['arrival_hold_seconds'], 30)
                self.assertEqual(plan['windows_motor_deadline_seconds'], 210)
                self.assertTrue(plan['operator_marks_required'])
                if trial['id'] == 'incline-measure-8':
                    self.assertEqual(plan['incline_ceiling_code'], 8)
                    self.assertEqual(plan['frames'][2], '0253020a085303')
            self.assertEqual(plan['frames'][0], '0253010000000080000000d203' if trial['id'] in ('ready-idle', 'ready-wake') else '02530100000000000000005203')
            self.assertEqual(plan['frames'][-1], '0253035003')
            if trial['return_hold_seconds']:
                self.assertEqual(plan['frames'][-2], '0253020a005b03')
        self.assertFalse(self.calls.exists())

    def test_timing_waits_for_physical_marks_and_preserves_ftms_source(self):
        self.check_timing_trial('incline-timing-8')

    def test_partial_timing_uses_level_four_and_waits_for_marks(self):
        self.check_timing_trial('incline-timing-4')
        motor = [r for r in json.loads(self.calls.read_text()) if r.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03',
                                '0253020a045f03', '0253020a005b03', '0253035003'])

    def test_retired_ready_trials_never_start_helper(self):
        for trial in ('ready-idle', 'ready-wake'):
            code, _, errors = self.trial(trial=trial)
            self.assertEqual(code, 2, errors)
            self.assertIn(b'READY trials retired', errors)
            self.assertFalse(self.calls.exists())
            self.assertFalse(self.log.exists())

    def test_standard_trial_refuses_sleep(self):
        code, _, errors = self.trial(mode='sleep')
        self.assertEqual(code, 1, errors)
        self.assertFalse(any(r.startswith('0253') for r in json.loads(self.calls.read_text())))

    def test_measurement_preserves_thirty_seconds_after_each_arrival(self):
        self.check_timing_trial('incline-measure-4', arrival_hold=30)

    def check_timing_trial(self, trial, arrival_hold=2):
        master, slave = pty.openpty()
        out_file, err_file = tempfile.TemporaryFile(), tempfile.TemporaryFile()  # files cannot fill up and block the CLI
        process = subprocess.Popen([*self.arguments, '--trial', trial, '--observe-ftms',
                                    '--confirm-motor', '--log', str(self.log)], stdin=slave,
                                   stdout=out_file, stderr=err_file, env=self.environment)
        def captured(file):
            file.seek(0)
            return file.read()
        def wait_for(predicate):
            deadline = time.monotonic() + arrival_hold + 60
            while time.monotonic() < deadline:
                if self.log.exists() and predicate(self.events()):
                    return
                if process.poll() is not None:
                    self.fail('Timing trial exited before marks: ' + captured(err_file).decode())
                time.sleep(0.02)
            self.fail('Timed out waiting for trial event')
        try:
            approve(master, err_file)
            for stage, key in ((1, b'U'), (2, b'D')):
                wait_for(lambda rows: any(r['event'] == 'reported_target' and r.get('stage') == stage for r in rows))
                os.write(master, key)  # Arrival without a movement mark must be ignored.
                time.sleep(0.2)
                self.assertFalse(any(r.get('state') == 'attained_observed' and r.get('stage') == stage for r in self.events()))
                os.write(master, b'B')
                time.sleep(0.3)
                os.write(master, key)
                wait_for(lambda rows: any(r.get('state') == 'attained_observed' and r.get('stage') == stage for r in rows))
            process.wait(timeout=arrival_hold + 60)
            self.assertEqual(process.returncode, 0, captured(out_file).decode() + captured(err_file).decode())
            rows = self.events()
            arrivals = [r for r in rows if r.get('state') == 'attained_observed']
            self.assertEqual(len(arrivals), 2)
            self.assertTrue(all(r['motion_duration_ms'] >= 200 for r in arrivals))
            self.assertTrue(all(r['report_preceded_arrival_ms'] >= 400 for r in arrivals))
            returned = next(r for r in rows if r['event'] == 'target_requested' and r['stage'] == 2)
            stopped = next(r for r in rows if r['event'] == 'stop_requested')
            self.assertGreaterEqual(returned['monotonic_ms'] - arrivals[0]['monotonic_ms'], arrival_hold * 1000)
            self.assertGreaterEqual(stopped['monotonic_ms'] - arrivals[1]['monotonic_ms'], arrival_hold * 1000)
            self.assertTrue(rows[-1]['operator_arrival_marks_recorded'])
            ftms = next(r for r in rows if r['event'] == 'ftms_notification')
            self.assertFalse(ftms['physical_attainment_verified'])
            self.assertFalse(rows[-1]['physical_stop_verified'])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_speed_changes_and_return_are_observed_before_stop(self):
        code, _, errors = self.trial(trial='speed-12')
        self.assertEqual(code, 0, errors)
        motor = [r for r in json.loads(self.calls.read_text()) if r.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03',
                                '0253020c005d03', '0253020a005b03', '0253035003'])
        self.assertEqual([r['speed_code'] for r in self.events() if r['event'] == 'reported_target'], [10, 12, 10])

    def test_incline_codes_and_return_preserve_raw_semantics(self):
        code, _, errors = self.trial(trial='incline-1')
        self.assertEqual(code, 0, errors)
        targets = [r for r in self.events() if r['event'] == 'reported_target']
        self.assertEqual([r['incline_code'] for r in targets], [0, 1, 0])
        self.assertTrue(all(not r['physical_attainment_verified'] for r in targets))
        self.assertFalse(self.events()[-1]['physical_stop_verified'])

    def test_pause_observation_still_ends_with_explicit_stop(self):
        code, _, errors = self.trial(trial='pause')
        self.assertEqual(code, 0, errors)
        motor = [r for r in json.loads(self.calls.read_text()) if r.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03', '0253065503', '0253035003'])
        self.assertTrue(any(r.get('status_code') == 10 for r in self.events()))

    def test_stop_observer_allows_deceleration_and_three_normal_reports(self):
        code, _, errors = self.trial(mode='delayed-stop')
        self.assertEqual(code, 0, errors)
        rows = self.events()
        stop = next(r for r in rows if r['event'] == 'stop_requested')
        normal = [r for r in rows if r.get('status_code') == 0 and r['monotonic_ms'] > stop['monotonic_ms']]
        self.assertEqual(len(normal), 3)
        self.assertGreater(normal[-1]['monotonic_ms'] - stop['monotonic_ms'], 5000)

    def test_batch_dry_plan_and_fault_halt_after_first_trial(self):
        arguments = list(self.arguments)
        arguments[1] = 'control-batch'
        result = subprocess.run([*arguments, '--dry-run'], env=self.environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        ids = [r['trial'] for r in json.loads(result.stdout)['trials']]
        self.assertEqual(ids, ['speed-50', 'speed-60', *['incline-' + str(i) for i in range(1, 9)], 'pause'])
        self.assertFalse(self.calls.exists())
        self.environment['MOTOR_MODE'] = 'batch-fault'
        directory = self.directory / 'batch'
        master, slave = pty.openpty()
        process = subprocess.Popen([*arguments, '--confirm-batch', '--log', str(directory)], stdin=slave,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            os.write(master, b'BATCH\n')
            output, errors = process.communicate(timeout=45)
            self.assertEqual(process.returncode, 1, output.decode() + errors.decode())
            self.assertIn(b'Batch halted at speed-60', errors)
            self.assertEqual({p.name for p in directory.iterdir()}, {'speed-50.jsonl', 'speed-60.jsonl'})
            first = [json.loads(r) for r in (directory / 'speed-50.jsonl').read_text().splitlines()]
            self.assertTrue(first[-1]['sequence_observed'])
            second = [json.loads(r) for r in (directory / 'speed-60.jsonl').read_text().splitlines()]
            self.assertFalse(any(r['event'] == 'start_requested' for r in second))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def linux(self, backend_report='linux-bluez'):
        helper = self.executable('fake-linux-helper', Path(self.environment['TREADMILL_POWERSHELL']).read_text().split('\n', 1)[1])
        self.environment.update(TREADMILL_LINUX_HELPER=str(helper), MOTOR_BACKEND=backend_report)
        self.arguments = [BINARY, 'control-test', '--backend', 'linux', '--device', ADDRESS, '--expected-name', 'FS-TEST']

    def test_linux_backend_runs_the_same_bounded_trial_through_its_helper(self):
        self.linux()
        code, _, errors = self.trial()
        self.assertEqual(code, 0, errors)
        motor = [value for value in json.loads(self.calls.read_text()) if value.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03', '0253035003'])
        self.assertEqual(json.loads(Path(str(self.calls) + '.argv').read_text()),
                         ['--device', ADDRESS, '--expected-name', 'FS-TEST', '--confirm-motor',
                          '--trial', 'start-stop', '--timeout', '12'])
        events = self.events()
        self.assertEqual(next(row for row in events if row['event'] == 'trial_plan')['backend'], 'linux-bluez')
        self.assertTrue(events[-1]['sequence_observed'])
        self.assertFalse(events[-1]['physical_stop_verified'])

    def test_linux_backend_can_observe_ftms_and_requires_the_helper_to_confirm_it(self):
        self.linux()
        code, _, errors = self.trial_with(['--observe-ftms'])
        self.assertEqual(code, 0, errors)
        events = self.events()
        self.assertTrue(any(row['event'] == 'ftms_notification' for row in events))
        reported = [row for row in events if row['event'] == 'distance_reported']
        self.assertEqual(len(reported), 1)
        self.assertGreater(reported[0]['distance_m'], 0)
        self.assertIn(b'Distance (reported by the treadmill)', errors)
        self.assertIn('--observe-ftms', json.loads(Path(str(self.calls) + '.argv').read_text()))
        refused = subprocess.run([BINARY, 'status', '--backend', 'linux', '--observe-ftms'], capture_output=True, env=self.environment)
        self.assertEqual(refused.returncode, 2)
        self.assertIn(b'requires control trials or hardware workouts', refused.stderr)

    def trial_with(self, extra, mode='normal'):
        self.environment['MOTOR_MODE'] = mode
        master, slave = pty.openpty()
        process = subprocess.Popen([*self.arguments, '--trial', 'start-stop', '--confirm-motor', '--log', str(self.log), *extra],
                                   stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            early = approve(master, process.stderr)
            output, errors = process.communicate(timeout=180)
            return process.returncode, output, early + errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    # --- running mode (Linux backend) ----------------------------------------------------------
    def stage(self, trial, env=None, actions=None, timeout=150, validated=None, keep_helper=False, display='match'):
        """Run a staged running-mode trial against the fake peer; returns (exit code, stderr).
        After a clean stage the CLI asks what the console display showed: 'match' answers with the stage speed."""
        if actions is None and display is not None:
            answer = ('%.1f' % float(trial[4:]) if display == 'match' else display) + '\n'
            actions = lambda send, wait_for: (wait_for('display_check_requested'), send(answer.encode()))
        if not keep_helper:
            self.linux()
        state = self.directory / 'state'
        if validated is not None:
            state.mkdir(exist_ok=True)
            (state / 'running-validated.json').write_text(json.dumps({'validated_kmh': validated}))
        self.environment.update(env or {})
        master, slave = pty.openpty()
        out_file, err_file = tempfile.TemporaryFile(), tempfile.TemporaryFile()  # files cannot fill up and block the CLI
        process = subprocess.Popen([*self.arguments, '--trial', trial, '--running-mode', '--confirm-motor', '--log', str(self.log)],
                                   stdin=slave, stdout=out_file, stderr=err_file, env=self.environment)
        def captured(file):
            file.seek(0)
            return file.read()
        started = time.monotonic()
        def wait_for(name, count=1):
            while time.monotonic() - started < timeout:
                if self.log.exists() and sum(1 for row in self.events() if row['event'] == name) >= count:
                    return
                if process.poll() is not None:
                    break
                time.sleep(0.02)
            tail = [json.dumps(r)[:230] for r in self.events()[-25:]] if self.log.exists() else []
            self.fail('Never saw %s (exit %s); last events:\n%s; stderr: %s' % (
                name, process.poll(), '\n'.join(tail), captured(err_file).decode(errors='replace')[-1500:]))
        try:
            approve(master, err_file)
            if actions:
                actions(lambda data: os.write(master, data), wait_for)
            process.wait(timeout=timeout)
            return process.returncode, captured(err_file)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def validated_kmh(self):
        path = self.directory / 'state' / 'running-validated.json'
        return json.loads(path.read_text())['validated_kmh'] if path.exists() else None

    def test_running_stages_unlock_in_order_and_unknown_stages_are_refused(self):
        self.linux()
        for trial, message in (('run-8', b'not unlocked: validated so far is 6 km/h, so the next stage is run-7'),
                               ('run-12', b'next stage is run-7'), ('run-9', b'Unknown running stage')):
            with self.subTest(trial=trial):
                result = subprocess.run([*self.arguments, '--trial', trial, '--running-mode', '--dry-run'], capture_output=True, env=self.environment)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
        result = subprocess.run([*self.arguments, '--trial', 'run-7', '--dry-run'], capture_output=True, env=self.environment)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'need --running-mode', result.stderr)
        plan = subprocess.run([*self.arguments, '--trial', 'run-7', '--running-mode', '--dry-run'], capture_output=True, text=True, env=self.environment)
        self.assertEqual(plan.returncode, 0, plan.stderr)
        data = json.loads(plan.stdout)
        self.assertEqual((data['profile'], data['speed_ceiling_code'], data['running_validated_kmh']), ('running', 70, 6.0))
        self.assertEqual([step['speed_code'] for step in data['workout_steps']], [60, 70, 60, 10])
        self.assertFalse(self.calls.exists())

    def test_running_options_are_linux_only_and_pass_the_profile_and_ftms_to_the_helper(self):
        windows = subprocess.run([BINARY, 'control-test', '--backend', 'windows', '--device', ADDRESS, '--expected-name', 'FS-TEST',
                                  '--running-mode', '--dry-run'], capture_output=True, env=self.environment)
        self.assertEqual(windows.returncode, 2)
        self.assertIn(b'Linux-backend', windows.stderr)
        code, _ = self.stage('run-7')
        self.assertEqual(code, 0)
        argv = json.loads(Path(str(self.calls) + '.argv').read_text())
        self.assertIn('--observe-ftms', argv)
        self.assertEqual(argv[argv.index('--profile') + 1], 'running')

    def test_a_completed_stage_with_agreeing_ftms_unlocks_exactly_the_next_stage(self):
        code, errors = self.stage('run-7')
        self.assertEqual(code, 0, errors)
        self.assertEqual(self.validated_kmh(), 7.0)
        events = self.events()
        result = next(row for row in events if row['event'] == 'running_stage_result')
        self.assertTrue(result['validated'])
        self.assertGreaterEqual(result['ftms_agreed_samples_at_top'], 10)
        self.assertEqual((result['display_answer'], result['display_matched']), ('7.0', True))
        self.assertEqual(result['validated_kmh_now'], 7.0)
        self.assertEqual(json.loads((self.directory / 'state' / 'running-validated.json').read_text())['display_kmh_reported_by_operator'], 7.0)
        self.assertTrue(events[-1]['sequence_observed'])
        motor = [f for f in json.loads(self.calls.read_text()) if f.startswith('0253')]
        self.assertEqual(motor[0], '02530100000000000000005203')
        self.assertEqual(motor[-1], '0253035003')
        # run-10 is still locked (next is run-8); a workout above 7 km/h is refused; one at 7 is accepted.
        locked = subprocess.run([*self.arguments, '--trial', 'run-10', '--running-mode', '--dry-run'], capture_output=True, env=self.environment)
        self.assertEqual(locked.returncode, 2)
        self.assertIn(b'next stage is run-8', locked.stderr)
        self.assertEqual(subprocess.run([*self.arguments, '--trial', 'run-8', '--running-mode', '--dry-run'], capture_output=True, env=self.environment).returncode, 0)
        self.assertEqual(self.run_running_workout(7.0, dry=True).returncode, 0)
        refused = self.run_running_workout(8.0, dry=True)
        self.assertEqual(refused.returncode, 2)
        self.assertIn(b'validated only up to 7 km/h', refused.stderr)

    def run_running_workout(self, speed, dry=False, running=True):
        workout = self.directory / 'running.xml'
        workout.write_text(f'''<rows version="1" id="fast" name="Fast" speedunit="km/h" inclineunit="percent" device="treadmill">
  <row name="Go" duration="00:01:00" forcespeed="1" speed="{speed}" inclination="1.375" />
</rows>''')
        arguments = [BINARY, 'workout', 'run', str(workout), '--backend', 'linux', '--device', 'DD:87:70:39:74:20',
                     '--expected-name', 'FS-0F7A32', *(['--running-mode'] if running else []), *(['--dry-run'] if dry else [])]
        return subprocess.run(arguments, capture_output=True, env=self.environment, stdin=subprocess.DEVNULL)

    def test_running_speeds_need_the_flag_and_walking_workouts_stay_walking(self):
        self.linux()
        no_flag = self.run_running_workout(7.0, dry=True, running=False)
        self.assertEqual(no_flag.returncode, 2)
        self.assertIn(b'add --running-mode', no_flag.stderr)
        self.assertEqual(self.run_running_workout(5.5, dry=True, running=False).returncode, 0)
        tall = self.run_running_workout(12.1, dry=True)
        self.assertEqual(tall.returncode, 2)
        self.assertIn(b'1.0', tall.stderr)

    def test_a_stage_unlocks_only_when_the_operator_reports_the_matching_display(self):
        # FTMS agrees in every case (it is the same module's number); only the operator's reading of the console can validate.
        import shutil
        for number, (answer, expected) in enumerate((('7.1', True), ('7.2', False), ('', False), ('fast', False))):
            with self.subTest(answer=answer):
                shutil.rmtree(self.directory / 'state', ignore_errors=True)
                self.log = self.directory / f'display-{number}.jsonl'
                self.calls.unlink(missing_ok=True)
                code, errors = self.stage('run-7', display=answer)
                self.assertEqual(code, 0, errors)  # the run itself is fine either way
                result = next(row for row in self.events() if row['event'] == 'running_stage_result')
                self.assertEqual(result['display_answer'], answer)
                self.assertEqual((result['display_matched'], result['validated']), (expected, expected))
                self.assertEqual(self.validated_kmh(), 7.0 if expected else None)
                self.assertIn(b'DISPLAY', errors)

    def test_running_mode_refuses_the_unexplained_unit_tag_before_any_motor_frame(self):
        code, _ = self.stage('run-7', env={'MOTOR_MODE': 'unknown-tag'}, display=None)
        self.assertNotEqual(code, 0)
        self.assertEqual([f for f in json.loads(self.calls.read_text()) if f.startswith('0253')], [])
        self.assertIn('not the running-mode report', next(row for row in self.events() if row['event'] == 'fault')['reason'])

    def test_a_stage_warns_the_operator_to_watch_the_display_before_the_belt_starts(self):
        self.linux()
        master, slave = pty.openpty()
        process = subprocess.Popen([*self.arguments, '--trial', 'run-7', '--running-mode', '--confirm-motor', '--log', str(self.log)],
                                   stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            early = approve(master, process.stderr, (b'NO\n',))
            _, errors = process.communicate(timeout=30)
            errors = early + errors
        finally:
            os.close(master); os.close(slave)
        self.assertIn(b'WATCH THE CONSOLE DISPLAY', errors)
        self.assertIn(b'same module', errors)
        self.assertNotIn(b'independent', errors)
        self.assertFalse(self.calls.exists())

    def test_a_stage_is_stopped_when_ftms_speed_disagrees_and_nothing_is_unlocked(self):
        code, _ = self.stage('run-7', env={'MOTOR_FTMS': 'mismatch'}, display=None)
        self.assertNotEqual(code, 0)
        self.assertIsNone(self.validated_kmh())
        events = self.events()
        stop = next(row for row in events if row['event'] == 'stop_requested')
        self.assertIn('disagrees', stop['reason'])
        self.assertFalse(next(row for row in events if row['event'] == 'running_stage_result')['validated'])

    def test_a_stage_is_stopped_when_ftms_speed_is_not_observed(self):
        code, _ = self.stage('run-7', env={'MOTOR_FTMS': 'silent'}, display=None)
        self.assertNotEqual(code, 0)
        self.assertIsNone(self.validated_kmh())
        stop = next(row for row in self.events() if row['event'] == 'stop_requested')
        self.assertIn('cannot be observed', stop['reason'])

    def test_running_mode_with_the_walking_limit_report_refuses_before_any_motor_frame(self):
        self.linux()
        # The fake answers walking limits unless the helper was started with the running profile; hide that to emulate a
        # console left in walking mode.
        self.environment['MOTOR_MODE'] = 'normal'
        source = Path(self.environment['TREADMILL_LINUX_HELPER']).read_text().replace("running_profile = argument('-Profile', '--profile') == 'running'", 'running_profile = False')
        walking = self.executable('walking-console-helper', source.split('\n', 1)[1])
        self.environment['TREADMILL_LINUX_HELPER'] = str(walking)
        code, _ = self.stage('run-7', keep_helper=True, display=None)
        self.assertNotEqual(code, 0)
        self.assertEqual([f for f in json.loads(self.calls.read_text()) if f.startswith('0253')], [])
        fault = next(row for row in self.events() if row['event'] == 'fault')
        self.assertIn('not the running-mode report', fault['reason'])

    def test_a_stop_from_high_speed_gets_a_longer_window_for_the_deceleration(self):
        # Operator stop at 10 km/h; the fake keeps reporting STOPPING for 14 s. The old fixed 10 s window would fault.
        def actions(send, wait_for):
            wait_for('reported_target', 2)
            time.sleep(1.0)
            send(b'Q')
        code, _ = self.stage('run-10', env={'MOTOR_STOP_DELAY': '14'}, actions=actions, validated=8)
        self.assertNotEqual(code, 0)  # an operator stop is never a successful stage
        events = self.events()
        self.assertIn('Operator stop', next(row for row in events if row['event'] == 'stop_requested')['reason'])
        self.assertFalse(any(row['event'] == 'fault' and 'Stop observation deadline' in row.get('reason', '') for row in events))
        self.assertTrue(events[-1]['normal_status_after_stop'])
        self.assertIsNone(None if self.validated_kmh() == 8 else self.validated_kmh())  # still 8: an aborted stage unlocks nothing

    def test_linux_backend_refuses_a_report_from_another_backend(self):
        self.linux(backend_report='windows-winrt')
        code, _, _ = self.trial()
        self.assertNotEqual(code, 0)
        self.assertFalse(self.events()[-1]['sequence_observed'])

    def test_linux_backend_without_helper_refuses_before_any_log_or_write(self):
        local = self.directory / 'treadmill'
        local.write_bytes(Path(BINARY).read_bytes())
        local.chmod(0o755)
        self.environment['TREADMILL_LINUX_HELPER'] = str(self.directory / 'missing-helper')
        self.arguments = [str(local), 'control-test', '--backend', 'linux', '--device', ADDRESS, '--expected-name', 'FS-TEST']
        code, _, errors = self.trial()
        self.assertEqual(code, 2)
        self.assertIn(b'Linux Bluetooth helper is missing', errors)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.calls.exists())

    def test_linux_backend_has_no_pause_ready_or_operator_timed_trials(self):
        self.linux()
        for trial in ('pause', 'ready-idle', 'incline-timing-4', 'incline-measure-8'):
            with self.subTest(trial=trial):
                code, _, errors = self.trial(trial=trial)
                self.assertEqual(code, 2)
                self.assertIn(b'not available on the Linux backend', errors)
                self.assertFalse(self.calls.exists())

    def test_linux_workout_runs_ordered_targets_and_stops(self):
        self.linux()
        code, _, errors = self.workout(backend='linux')
        self.assertEqual(code, 0, errors)
        motor = [frame for frame in json.loads(self.calls.read_text()) if frame.startswith('0253')]
        self.assertEqual(motor, ['02530100000000000000005203', '0253020a005b03',
                                 '02530214004503', '02530214014403', '0253035003'])
        argv = json.loads(Path(str(self.calls) + '.argv').read_text())
        self.assertEqual(argv[argv.index('--trial') + 1], 'workout')
        self.assertEqual(argv[argv.index('--target-sequence') + 1], '10:0,20:0,20:1')

    def test_linux_workout_safety_fault_stops_before_later_targets(self):
        self.linux()
        code, _, _ = self.workout(mode='workout-fault', backend='linux')
        self.assertEqual(code, 1)
        motor = [frame for frame in json.loads(self.calls.read_text()) if frame.startswith('0253')]
        self.assertNotIn('02530214014403', motor)
        self.assertEqual(motor[-1], '0253035003')

    def paused_workout(self, backend='linux', extra=()):
        """A Linux workout during which the fake console is paused 4 s into the climb, for 4 s, then Play restarts it at 1.0 km/h, incline 0."""
        if backend == 'linux':
            self.linux()
        self.environment['MOTOR_CONSOLE_PAUSE'] = '2,4,4'
        workout = self.directory / 'pause-w1.xml'
        workout.write_text('''<rows version="1" id="pause-w1" name="Pause W1" speedunit="km/h" inclineunit="percent" device="treadmill">
  <row name="Baseline" duration="00:00:01" forcespeed="1" speed="1" inclination="0" />
  <row name="Climb" duration="00:00:12" forcespeed="1" speed="3" inclination="2.75" />
  <row name="Last" duration="00:00:02" forcespeed="1" speed="2" inclination="0" />
</rows>''')
        arguments = [BINARY, 'workout', 'run', str(workout), '--backend', backend, '--device', 'DD:87:70:39:74:20',
                     '--expected-name', 'FS-0F7A32', '--confirm-motor', '--log', str(self.log), *extra]
        master, slave = pty.openpty()
        process = subprocess.Popen(arguments, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            early = approve(master, process.stderr)
            output, errors = process.communicate(timeout=90)
            return process.returncode, output, early + errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_a_console_pause_freezes_the_plan_and_play_ramps_back_gradually(self):
        code, _, errors = self.paused_workout(extra=['--observe-ftms'])
        self.assertEqual(code, 0, errors)
        self.assertFalse(Path(str(self.calls) + '.paused').exists(), 'a target was sent while the console was paused')
        events = self.events()
        names = [row['event'] for row in events]
        self.assertEqual(names.count('console_pause'), 1)
        self.assertEqual(names.count('console_play'), 1)
        self.assertEqual(names.count('console_resumed'), 1)
        ramp = [(row['speed_code'], row['incline_code']) for row in events if row['event'] == 'resume_target_requested']
        self.assertEqual(ramp, [(15, 1), (20, 2), (25, 2), (30, 2)])
        self.assertIn(b'Paused on the console', errors)
        argv = json.loads(Path(str(self.calls) + '.argv').read_text())
        self.assertEqual(argv[argv.index('--pause-allowance') + 1], '900')
        # The climb's 12 s are held in all: before the pause plus after the return, not counting the pause and the ramp.
        climb = next(row for row in events if row['event'] == 'reported_target' and row['stage'] == 1)
        pause = next(row for row in events if row['event'] == 'console_pause')
        resumed = next(row for row in events if row['event'] == 'console_resumed')
        last = next(row for row in events if row['event'] == 'target_requested' and row['stage'] == 2)
        held = (pause['monotonic_ms'] - climb['monotonic_ms']) + (last['monotonic_ms'] - resumed['monotonic_ms'])
        self.assertAlmostEqual(held / 1000.0, 12, delta=1.0)
        self.assertEqual(events[-1]['console_pauses'], 1)
        self.assertTrue(events[-1]['sequence_observed'])
        # The fake's distance counter restarts at zero on Play; the reported distance adds both segments.
        odometers = [int.from_bytes(bytes.fromhex(row['frame_hex'])[4:7], 'little') for row in events
                     if row['event'] == 'ftms_notification' and row['frame_hex'].startswith('8c05')]
        distance = next(row for row in events if row['event'] == 'distance_reported')['distance_m']
        self.assertGreater(distance, max(odometers))

    def test_the_windows_bridge_still_ends_the_run_on_a_console_pause(self):
        code, _, _ = self.paused_workout(backend='windows')
        self.assertEqual(code, 1)
        events = self.events()
        self.assertFalse(any(row['event'] == 'console_pause' for row in events))
        self.assertEqual(next(row for row in events if row['event'] == 'stop_requested')['reason'], 'Unexpected motor feedback')

    def manual_workout(self, strategy, commands, wait_stage=1):
        """Run a 3-step Linux workout with --manual-control, sending JSON command lines once step `wait_stage` is reported."""
        self.linux()
        workout = self.directory / 'manual-w1.xml'
        workout.write_text('''<rows version="1" id="manual-w1" name="Manual W1" speedunit="km/h" inclineunit="percent" device="treadmill">
  <row name="Baseline" duration="00:00:01" forcespeed="1" speed="1" inclination="0" />
  <row name="First" duration="00:00:12" forcespeed="1" speed="2" inclination="0" />
  <row name="Second" duration="00:00:12" forcespeed="1" speed="3" inclination="0" />
</rows>''')
        arguments = [BINARY, 'workout', 'run', str(workout), '--backend', 'linux', '--device', 'DD:87:70:39:74:20',
                     '--expected-name', 'FS-0F7A32', '--confirm-motor', '--log', str(self.log), '--manual-control']
        if strategy: arguments += ['--manual-strategy', strategy]
        master, slave = pty.openpty()
        process = subprocess.Popen(arguments, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            early = approve(master, process.stderr)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if self.log.exists() and any(row['event'] == 'reported_target' and row['stage'] == wait_stage for row in self.events()):
                    break
                time.sleep(0.05)
            for command in commands:
                os.write(master, (command if isinstance(command, bytes) else json.dumps(command).encode()) + b'\n')
                time.sleep(0.4)
            output, errors = process.communicate(timeout=60)
            return process.returncode, output, early + errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def motor_targets(self):
        return [frame for frame in json.loads(self.calls.read_text()) if frame.startswith('025302')]

    def results(self):
        return [(row.get('seq'), row['result'], row.get('speed_code'), row.get('incline_code'))
                for row in self.events() if row['event'] == 'manual_command_result']

    def test_manual_takeover_is_the_default_and_the_plan_stops_setting_targets(self):
        code, _, errors = self.manual_workout(None, [{'seq': 1, 'cmd': 'speed_up'}, {'seq': 2, 'cmd': 'speed_up'},
                                                      {'seq': 3, 'cmd': 'incline_up'}, {'seq': 4, 'cmd': 'bogus'}, b'not json'])
        self.assertEqual(code, 0, errors)
        self.assertEqual(self.results(), [(1, 'applied', 21, 0), (2, 'applied', 22, 0), (3, 'applied', 22, 1),
                                          (4, 'refused', 22, 1), (None, 'refused', None, None)])
        # Baseline (10:0), First (20:0), the three manual targets; the plan's Second step (30:0) is never sent.
        self.assertEqual([frame[6:10] for frame in self.motor_targets()], ['0a00', '1400', '1500', '1600', '1601'])
        argv = json.loads(Path(str(self.calls) + '.argv').read_text())
        self.assertEqual(argv[argv.index('--manual-max-speed') + 1], '60')
        self.assertEqual(next(row for row in self.events() if row['event'] == 'trial_plan')['manual_strategy'], 'takeover')
        received = [row for row in self.events() if row['event'] == 'manual_command_received']
        self.assertEqual([row.get('seq') for row in received], [1, 2, 3, 4, None])

    def test_manual_offset_shifts_this_and_later_steps(self):
        code, _, errors = self.manual_workout('offset', [{'seq': 1, 'cmd': 'speed_up'}, {'seq': 2, 'cmd': 'speed_up'}])
        self.assertEqual(code, 0, errors)
        self.assertEqual([frame[6:10] for frame in self.motor_targets()], ['0a00', '1400', '1500', '1600', '2000'])

    def test_manual_hold_lasts_until_the_next_plan_step(self):
        code, _, errors = self.manual_workout('hold', [{'seq': 1, 'cmd': 'speed_up'}, {'seq': 2, 'cmd': 'incline_up'}])
        self.assertEqual(code, 0, errors)
        self.assertEqual([frame[6:10] for frame in self.motor_targets()], ['0a00', '1400', '1500', '1501', '1e00'])

    def test_manual_limits_refuse_instead_of_clamping_and_stop_ends_the_run(self):
        code, _, errors = self.manual_workout('hold', [{'seq': 1, 'cmd': 'incline_down'}, {'seq': 2, 'cmd': 'stop'}])
        self.assertEqual(code, 1)
        self.assertEqual(self.results()[0], (1, 'refused', 20, 0))
        self.assertEqual(self.motor_targets()[-1][6:10], '1400')
        self.assertEqual([f for f in json.loads(self.calls.read_text()) if f.startswith('0253')][-1], '0253035003')

    def test_manual_control_needs_a_linux_hardware_workout(self):
        self.linux()
        for extra in (['--manual-control'], ['--manual-strategy', 'offset']):
            refused = subprocess.run([BINARY, 'control-test', '--backend', 'linux', '--device', ADDRESS, '--expected-name', 'FS-TEST',
                                      '--trial', 'start-stop', '--dry-run', *extra], capture_output=True, text=True, env=self.environment)
            self.assertEqual(refused.returncode, 2)
        bad = subprocess.run([BINARY, 'workout', 'run', 'uphill-walk-20', '--dry-run', '--manual-control', '--manual-strategy', 'sideways'],
                             capture_output=True, text=True, env=self.environment)
        self.assertEqual(bad.returncode, 2)
        self.assertIn('offset, hold or takeover', bad.stderr)
        ok = subprocess.run([BINARY, 'workout', 'run', 'uphill-walk-20', '--dry-run', '--manual-control'], capture_output=True, text=True, env=self.environment)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual((json.loads(ok.stdout)['manual_strategy'], json.loads(ok.stdout)['manual_max_speed_code']), ('takeover', 60))

    # --- wait-for-wake (Linux backend) -------------------------------------------------
    def waiting(self, extra, wake_after=4, resleep=None, key=False, early=b'', actions=None, timeout=90):
        """Run start-stop with --wait-wake against a fake peer that sleeps for the first polls."""
        self.linux()
        self.environment.update(MOTOR_MODE='normal', MOTOR_WAKE_AFTER=str(wake_after))
        if resleep:
            self.environment['MOTOR_RESLEEP'] = resleep
        if key:
            self.environment['MOTOR_WAKE_KEY'] = '1'
        master, slave = pty.openpty()
        out_file, err_file = tempfile.TemporaryFile(), tempfile.TemporaryFile()  # files cannot fill up and block the CLI
        process = subprocess.Popen([*self.arguments, '--trial', 'start-stop', '--confirm-motor', '--log', str(self.log), *extra],
                                   stdin=slave, stdout=out_file, stderr=err_file, env=self.environment)
        def captured(file):
            file.seek(0)
            return file.read()
        started = time.monotonic()
        def wait_for(name, count=1):
            while time.monotonic() - started < timeout:
                if self.log.exists() and sum(1 for row in self.events() if row['event'] == name) >= count:
                    return
                if process.poll() is not None:
                    break
                time.sleep(0.02)
            self.fail('Never saw event %s x%d' % (name, count))
        try:
            approve(master, err_file)
            os.write(master, early)  # typed after the approval, before the wake: must not count as the start ENTER
            if actions:
                actions(lambda data: os.write(master, data), wait_for)
            process.wait(timeout=timeout)
            return process.returncode, captured(err_file)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def motor_frames(self):
        return [value for value in json.loads(self.calls.read_text())
                if value.startswith('0253')] if self.calls.exists() else []

    def first(self, name):
        return next(row for row in self.events() if row['event'] == name)

    def test_wait_wake_unattended_sends_nothing_until_grace_ends_then_starts(self):
        code, errors = self.waiting(['--wait-wake', '1'])
        self.assertEqual(code, 0, errors)
        grace, start = self.first('wake_grace_started'), self.first('start_requested')
        self.assertGreaterEqual(start['monotonic_ms'] - grace['monotonic_ms'], 5000)
        first_motor = next(row for row in self.events() if row['event'] == 'write_requested' and row['frame_hex'].startswith('0253'))
        self.assertGreaterEqual(first_motor['monotonic_ms'] - grace['monotonic_ms'], 5000)
        self.assertLess(self.first('waiting_for_wake')['monotonic_ms'], grace['monotonic_ms'])
        self.assertTrue(self.events()[-1]['sequence_observed'])

    def test_wait_wake_unattended_q_during_grace_cancels_without_motor_frames(self):
        code, _ = self.waiting(['--wait-wake', '1'], actions=lambda send, wait_for: (wait_for('wake_grace_started'), send(b'Q')))
        self.assertNotEqual(code, 0)
        self.assertEqual(self.motor_frames(), [])
        self.assertFalse(any(row['event'] == 'start_requested' for row in self.events()))

    def test_wait_wake_confirm_starts_only_on_enter_and_ignores_early_enter(self):
        def actions(send, wait_for):
            wait_for('awake_waiting_for_confirmation')
            time.sleep(2)
            self.assertEqual(self.motor_frames(), [])
            self.assertFalse(any(row['event'] == 'start_requested' for row in self.events()))
            send(b'\n')
        code, errors = self.waiting(['--wait-wake', '1', '--wait-wake-confirm'], early=b'\n\n', actions=actions)
        self.assertEqual(code, 0, errors)
        self.assertEqual(sum(row['event'] == 'awake_waiting_for_confirmation' for row in self.events()), 1)
        self.assertLess(self.first('start_confirmed')['monotonic_ms'], self.first('start_requested')['monotonic_ms'])
        self.assertGreater(self.first('start_confirmed')['monotonic_ms'], self.first('awake_waiting_for_confirmation')['monotonic_ms'])

    def test_wait_wake_confirm_asks_again_if_the_console_sleeps_before_enter(self):
        def actions(send, wait_for):
            wait_for('awake_waiting_for_confirmation')
            wait_for('wake_lost')
            wait_for('awake_waiting_for_confirmation', 2)
            self.assertFalse(any(row['event'] == 'start_requested' for row in self.events()))
            send(b'\n')
        code, errors = self.waiting(['--wait-wake', '1', '--wait-wake-confirm'], resleep='10,6', actions=actions)
        self.assertEqual(code, 0, errors)

    def test_wait_wake_confirm_enter_does_not_survive_a_sleep(self):
        # ENTER immediately after the prompt, then the console falls asleep: waking later must not start the belt.
        def actions(send, wait_for):
            wait_for('awake_waiting_for_confirmation')
            send(b'\n')
            wait_for('start_confirmed')
            wait_for('wake_lost')
            wait_for('awake_waiting_for_confirmation', 2)
            time.sleep(2)
            self.assertFalse(any(row['event'] == 'start_requested' for row in self.events()))
            self.assertEqual(self.motor_frames(), [])
            send(b'\n')
        code, errors = self.waiting(['--wait-wake', '1', '--wait-wake-confirm'], resleep='8,6', actions=actions)
        self.assertEqual(code, 0, errors)
        self.assertEqual(sum(row['event'] == 'start_confirmed' for row in self.events()), 2)

    def test_wait_wake_key_detached_after_wake_faults_without_starting(self):
        code, _ = self.waiting(['--wait-wake', '1'], key=True)
        self.assertNotEqual(code, 0)
        self.assertEqual(self.motor_frames(), [])
        self.assertTrue(any(row['event'] == 'fault' for row in self.events()))

    def test_wait_wake_limit_expires_without_motor_frames(self):
        code, _ = self.waiting(['--wait-wake', '0.1'], wake_after=100000)
        self.assertEqual(code, 1)
        self.assertEqual(self.motor_frames(), [])
        fault = self.first('fault')
        self.assertIn('did not wake', fault['reason'])
        self.assertIn('waiting_for_wake', [row['event'] for row in self.events()])

    def test_wait_wake_options_are_validated_and_linux_only(self):
        self.linux()
        for extra, message in ((['--wait-wake-confirm'], b'requires --wait-wake'),
                               (['--wait-wake', '0'], b'0.05-30'), (['--wait-wake', '31'], b'0.05-30'),
                               (['--wait-wake', 'soon'], b'0.05-30')):
            with self.subTest(extra=extra):
                result = subprocess.run([*self.arguments, '--trial', 'start-stop', '--confirm-motor', '--log', str(self.log), *extra],
                                        capture_output=True, env=self.environment, stdin=subprocess.DEVNULL)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
        windows = subprocess.run([BINARY, 'control-test', '--backend', 'windows', '--device', ADDRESS, '--expected-name', 'FS-TEST',
                                  '--wait-wake', '1', '--dry-run'], capture_output=True, env=self.environment)
        self.assertEqual(windows.returncode, 2)
        self.assertIn(b'only available on the Linux backend', windows.stderr)
        plan = subprocess.run([*self.arguments, '--wait-wake', '2', '--wait-wake-confirm', '--dry-run'],
                              capture_output=True, text=True, env=self.environment)
        self.assertEqual(plan.returncode, 0, plan.stderr)
        self.assertEqual((json.loads(plan.stdout)['wait_wake_seconds'], json.loads(plan.stdout)['wait_wake_confirm']), (120, True))
        self.assertFalse(self.calls.exists())


if __name__ == '__main__':
    unittest.main()
