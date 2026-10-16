"""Offline CLI integration; no test invokes scan, info, or a hardware backend."""
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


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.log = Path(self.temporary.name) / 'session.jsonl'

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, *arguments):
        return subprocess.run([BINARY, *arguments], capture_output=True, text=True, timeout=15)

    def run_plan(self, *options):
        return self.command('workout', 'run', 'interval-demo', '--simulate', '--fast',
                            '--log', str(self.log), *options)

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def wait_starting(self, process):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and process.poll() is None:
            if self.log.exists() and '"state":"STARTING"' in self.log.read_text():
                return
            time.sleep(0.01)
        self.fail('CLI failed to enter STARTING within five seconds')

    def test_catalog_and_full_example(self):
        result = self.command('workout', 'list')
        self.assertEqual(result.returncode, 0, result.stderr)
        catalog = json.loads(result.stdout)
        core = {'uphill-walk-20': 1200, 'uphill-walk-60': 3600, 'interval-demo': 120, 'runpad-w1-discovery-demo': 310}
        self.assertEqual({row['id']: row['duration_seconds'] for row in catalog if row['id'] in core}, core)
        # Everything else in the catalog is a FIT training (see trainings/) and must be a valid, W1-sized plan.
        for row in catalog:
            if row['id'] not in core:
                self.assertTrue(row['path'].endswith('.fit'), row)
                self.assertTrue(20 * 60 <= row['duration_seconds'] <= 65 * 60, row)
        result = self.command('workout', 'run', 'uphill-walk-20', '--simulate', '--fast', '--log', str(self.log))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = self.events()
        self.assertEqual(events[-1]['state'], 'COMPLETED')
        self.assertEqual(events[-1]['elapsed_ms'], 1200000)
        self.assertEqual(events[-1]['reported']['speed_kmh'], 0)
        self.assertTrue(any(row.get('phase') == 'settled' for row in events))
        self.assertTrue(any(row['event'] == 'notification' for row in events))

    def test_fault_outcomes(self):
        expected = {'stale': 'STOP_UNCONFIRMED', 'disconnect': 'STOP_UNCONFIRMED',
                    'stop-failure': 'STOP_UNCONFIRMED', 'override': 'ABORTED',
                    'reject': 'ABORTED', 'no-ack': 'COMPLETED'}
        for fault, state in expected.items():
            with self.subTest(fault=fault):
                self.log.unlink(missing_ok=True)
                result = self.run_plan('--inject', fault)
                self.assertEqual(result.returncode, 0 if state == 'COMPLETED' else 1, result.stderr)
                self.assertEqual(self.events()[-1]['state'], state)
                if fault == 'disconnect':
                    self.assertFalse(self.events()[-1]['connected'])

    def test_hardware_gate_and_invalid_options(self):
        result = self.command('workout', 'run', 'interval-demo')
        self.assertEqual(result.returncode, 2)
        self.assertIn('interactive operator confirmation', result.stderr)
        self.assertEqual(self.command('info').returncode, 2)
        self.assertEqual(self.command('scan', '--timeout', '0').returncode, 2)
        self.assertEqual(self.command('workout', 'list', '--simulate').returncode, 2)

    def test_windows_w1_workout_dry_run_maps_the_full_bounded_schedule(self):
        result = self.command('workout', 'run', 'runpad-w1-discovery-demo', '--backend', 'windows',
                              '--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32', '--dry-run')
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertFalse(plan['motor_execution_enabled'])
        self.assertFalse(plan['physical_units_verified'])
        self.assertFalse(plan['physical_stop_verified'])
        self.assertEqual(plan['workout_id'], 'runpad-w1-discovery-demo')
        self.assertEqual(plan['windows_motor_deadline_seconds'], 499)
        self.assertEqual(plan['speed_ceiling_code'], 60)
        self.assertEqual(plan['incline_ceiling_code'], 8)
        steps = plan['workout_steps']
        self.assertEqual(len(steps), 23)
        self.assertEqual([step['speed_code'] for step in steps[:7]], [10, 20, 30, 40, 50, 60, 10])
        self.assertEqual([step['incline_code'] for step in steps[7:]], list(range(1, 9)) + list(range(7, -1, -1)))
        for step in steps:
            self.assertGreaterEqual(step['speed_code'], 10)
            self.assertLessEqual(step['speed_code'], 60)
            self.assertGreaterEqual(step['incline_code'], 0)
            self.assertLessEqual(step['incline_code'], 8)
            self.assertAlmostEqual(step['grade_percent_assumption'], step['incline_code'] * 11 / 8)
        self.assertEqual(plan['frames'][0], '02530100000000000000005203')
        self.assertEqual(plan['frames'][-1], '0253035003')
        self.assertEqual(len(plan['frames']), 25)  # READY/start, baseline, 22 changed targets, STOP.

    def test_protocol_diagnostics_cannot_start_without_approval_and_terminal(self):
        profile_path = Path(__file__).resolve().parent.parent / 'profiles' / 'unverified-example.json'
        result = self.command('status', '--profile', str(profile_path))
        self.assertEqual(result.returncode, 2)
        self.assertIn('--confirm-diagnostics', result.stderr)
        profile = json.loads(profile_path.read_text())
        profile['validation']['diagnostic_writes_confirmed'] = True
        reviewed = Path(self.temporary.name) / 'reviewed.json'
        reviewed.write_text(json.dumps(profile))
        result = subprocess.run([BINARY, 'status', '--profile', str(reviewed), '--confirm-diagnostics'],
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('interactive terminal', result.stderr)

    def test_declined_terminal_confirmation_never_creates_a_trace(self):
        source = Path(__file__).resolve().parent.parent / 'profiles' / 'unverified-example.json'
        profile = json.loads(source.read_text())
        profile['validation']['diagnostic_writes_confirmed'] = True
        reviewed = Path(self.temporary.name) / 'reviewed.json'
        reviewed.write_text(json.dumps(profile))
        master, slave = pty.openpty()
        process = subprocess.Popen([BINARY, 'status', '--profile', str(reviewed), '--confirm-diagnostics',
                                    '--log', str(self.log)], stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            os.write(master, b'NO\n')
            output, errors = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 2, output.decode() + errors.decode())
            self.assertIn('not confirmed', errors.decode())
            self.assertFalse(self.log.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_profile_preview_never_enables_hardware_execution(self):
        profile = Path(__file__).resolve().parent.parent / 'profiles' / 'unverified-example.json'
        result = self.command('workout', 'inspect', 'uphill-walk-20', '--profile', str(profile))
        self.assertEqual(result.returncode, 0, result.stderr)
        preview = json.loads(result.stdout)
        self.assertTrue(preview['preview_only'])
        self.assertFalse(preview['motor_execution_enabled'])
        self.assertFalse(preview['profile_motor_evidence_complete'])
        self.assertEqual(len(preview['stages']), 11)
        self.assertEqual(self.command('workout', 'run', 'uphill-walk-20', '--profile', str(profile)).returncode, 2)

    def test_simulator_status_has_explicit_backend(self):
        result = self.command('status', '--simulate')
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['backend'], 'simulator')
        self.assertFalse(report['running'])
        self.assertEqual(report['reported']['speed_kmh'], 0)
        self.assertEqual(self.command('status').returncode, 2)

    def test_log_never_overwrites_existing_file(self):
        self.log.write_text('keep this session')
        result = self.run_plan()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.log.read_text(), 'keep this session')

    def test_eof_aborts(self):
        result = subprocess.run([BINARY, 'workout', 'run', 'interval-demo', '--simulate',
                                 '--log', str(self.log)], stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.events()[-1]['state'], 'ABORTED')

    def test_signal_and_terminal_restoration(self):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([BINARY, 'workout', 'run', 'interval-demo', '--simulate',
                                    '--log', str(self.log)], stdin=slave,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.wait_starting(process)
            process.send_signal(signal.SIGTERM)
            output, errors = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 1, output.decode() + errors.decode())
            self.assertEqual(self.events()[-1]['state'], 'ABORTED')
            self.assertEqual(termios.tcgetattr(slave), before)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_single_key_stop_without_newline(self):
        master, slave = pty.openpty()
        process = subprocess.Popen([BINARY, 'workout', 'run', 'interval-demo', '--simulate',
                                    '--log', str(self.log)], stdin=slave,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.wait_starting(process)
            os.write(master, b's')
            output, errors = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 1, output.decode() + errors.decode())
            self.assertEqual(self.events()[-1]['message'], 'User stop')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    unittest.main()
