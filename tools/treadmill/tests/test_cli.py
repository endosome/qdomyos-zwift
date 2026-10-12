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
        self.assertEqual({row['id']: row['duration_seconds'] for row in json.loads(result.stdout)},
                         {'uphill-walk-20': 1200, 'interval-demo': 120})
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

    def test_hardware_gate_and_invalid_options(self):
        result = self.command('workout', 'run', 'interval-demo')
        self.assertEqual(result.returncode, 2)
        self.assertIn('disabled', result.stderr)
        self.assertEqual(self.command('info').returncode, 2)
        self.assertEqual(self.command('scan', '--timeout', '0').returncode, 2)
        self.assertEqual(self.command('workout', 'list', '--simulate').returncode, 2)

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
