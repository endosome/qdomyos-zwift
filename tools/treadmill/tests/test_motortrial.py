"""Bounded motor trial with a fake local BLE peer; no hardware I/O."""
import json
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ADDRESS = 'AA:BB:CC:DD:EE:FF'


class MotorTrialTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.log = self.directory / 'trial.jsonl'
        self.calls = self.directory / 'frames.json'
        self.environment = dict(os.environ, PATH=str(self.directory) + os.pathsep + os.environ['PATH'],
                                MOTOR_CALLS=str(self.calls), MOTOR_MODE='normal')
        self.executable('wslpath', 'import sys\nprint(sys.argv[-1])\n')
        helper = self.executable('fake-powershell', '''import json, os, sys, time
from pathlib import Path
mode = os.environ['MOTOR_MODE']
trial = sys.argv[sys.argv.index('-Trial') + 1]
if mode == 'batch-fault':
    mode = 'bounds' if trial == 'speed-60' else 'normal'
observe_ftms = '-ObserveFtms' in sys.argv
calls = []
moving = False
ready = False
ready_at = 0
stopped = False
speed, incline, paused = 10, 0, False
normal_at = 0
def send(line):
    print(line, flush=True)
def rx(payload):
    checksum = 0
    for byte in payload:
        checksum ^= byte
    send('RX\\t' + (bytes([2]) + payload + bytes([checksum,3])).hex())
send('IDENTITY\\tFITSHOW\\tFS-BT-C1\\t1.3.8')
send('READY\\tFS-TEST')
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
        rx(bytes.fromhex('50023c0a00') if mode != 'bounds' else bytes.fromhex('5002780a20'))
    elif payload == bytes.fromhex('5003'):
        rx(bytes.fromhex('5003080002'))
    elif payload[0] == 0x51:
        if not moving and not ready and not stopped and mode in ('sleep', 'key', 'unknown-disabled', 'sleep-stays'):
            rx(bytes.fromhex('510602') if mode in ('sleep', 'sleep-stays') else
               (bytes.fromhex('510601') if mode == 'key' else bytes.fromhex('5106')))
            continue
        if ready and mode == 'ready-reverted' and time.monotonic() - ready_at > 1:
            ready = False
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
        moving = True
        speed, incline = payload[2], payload[3]
        rx(bytes.fromhex('5302'))
        if mode == 'safety':
            rx(bytes.fromhex('510602'))
    elif payload == bytes.fromhex('5303'):
        stopped = True
        moving, paused, ready = False, False, False
        if mode == 'delayed-stop':
            normal_at = time.monotonic() + 4.4
        rx(bytes.fromhex('5303'))
    elif payload == bytes.fromhex('5306'):
        moving, paused = False, True
        rx(bytes.fromhex('5306'))
send(json.dumps(dict(command='control-test', backend='windows-winrt', status='complete',
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

    def trial(self, mode='normal', confirmation=b'MOTOR\n', terminate=False, trial='start-stop'):
        self.environment['MOTOR_MODE'] = mode
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([*self.arguments, '--trial', trial, '--confirm-motor', '--log', str(self.log)],
                                   stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        try:
            os.write(master, confirmation)
            if terminate:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if self.log.exists() and '"event":"start_requested"' in self.log.read_text():
                        process.send_signal(signal.SIGTERM)
                        break
                    time.sleep(0.01)
                else:
                    self.fail('Motor request never reached fake peer')
            output, errors = process.communicate(timeout=60)
            self.assertEqual(termios.tcgetattr(slave), before)
            return process.returncode, output, errors
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
        code, _, _ = self.trial(confirmation=b'NO\n')
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
        process = subprocess.Popen([*self.arguments, '--trial', trial, '--observe-ftms',
                                    '--confirm-motor', '--log', str(self.log)], stdin=slave,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        def wait_for(predicate):
            deadline = time.monotonic() + arrival_hold + 15
            while time.monotonic() < deadline:
                if self.log.exists() and predicate(self.events()):
                    return
                if process.poll() is not None:
                    self.fail('Timing trial exited before marks: ' + process.stderr.read().decode())
                time.sleep(0.02)
            self.fail('Timed out waiting for trial event')
        try:
            os.write(master, b'MOTOR\n')
            for stage, key in ((1, b'U'), (2, b'D')):
                wait_for(lambda rows: any(r['event'] == 'reported_target' and r.get('stage') == stage for r in rows))
                os.write(master, key)  # Arrival without a movement mark must be ignored.
                time.sleep(0.2)
                self.assertFalse(any(r.get('state') == 'attained_observed' and r.get('stage') == stage for r in self.events()))
                os.write(master, b'B')
                time.sleep(0.3)
                os.write(master, key)
                wait_for(lambda rows: any(r.get('state') == 'attained_observed' and r.get('stage') == stage for r in rows))
            output, errors = process.communicate(timeout=arrival_hold + 15)
            self.assertEqual(process.returncode, 0, output.decode() + errors.decode())
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


if __name__ == '__main__':
    unittest.main()
