"""Offline bridge tests: fake child processes, no Windows or Bluetooth I/O."""
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
SOURCE = Path(__file__).resolve().parent.parent


class WindowsBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.log = self.directory / 'report.json'
        self.calls = self.directory / 'calls.json'
        self.environment = dict(os.environ, PATH=str(self.directory) + os.pathsep + os.environ['PATH'],
                                BRIDGE_CALLS=str(self.calls), BRIDGE_MODE='complete')
        self.executable('wslpath', 'import sys\nprint(sys.argv[-1])\n')
        helper = self.executable('fake-powershell', '''import json, os, sys, time
from pathlib import Path
arguments = sys.argv[1:]
Path(os.environ['BRIDGE_CALLS']).write_text(json.dumps(arguments))
command = arguments[arguments.index('-Command') + 1]
mode = os.environ['BRIDGE_MODE']
if mode == 'wait':
    line = sys.stdin.readline()
    if line != 'QUIT\\n':
        sys.exit(3)
report = dict(command=command, backend='windows-winrt', status='complete',
              motor_execution_enabled=False, notification_cleanup_status='Success')
report['status_heart_rate_zero'] = '-StatusHeartRateZero' in arguments
report['status_data_sport'] = '-StatusDataSport' in arguments
report['measure_sleep_timer'] = '-MeasureSleepTimer' in arguments
if mode == 'variant-missing':
    report.pop('status_heart_rate_zero')
if mode == 'motor':
    report['motor_execution_enabled'] = True
if mode == 'cleanup':
    report['notification_cleanup_status'] = 'unconfirmed'
if mode == 'malformed':
    print('invalid json')
else:
    print(json.dumps(report))
''')
        self.environment['TREADMILL_POWERSHELL'] = str(helper)
        profile = json.loads((SOURCE / 'profiles/unverified-example.json').read_text())
        profile['validation']['diagnostic_writes_confirmed'] = True
        profile['identity']['name'] = 'Name with spaces; $(never execute)'
        self.profile = self.directory / 'profile.json'
        self.profile.write_text(json.dumps(profile))

    def tearDown(self):
        self.temporary.cleanup()

    def executable(self, name, contents):
        path = self.directory / name
        path.write_text('#!' + sys.executable + '\n' + contents)
        path.chmod(0o755)
        return path

    def command(self, *arguments):
        return subprocess.run([BINARY, *arguments], env=self.environment, capture_output=True,
                              text=True, timeout=10)

    def probe(self, mode='complete', confirmation=b'NONMOTOR\n', stop=None, identity_only=False, timeout=None, zero_heart_rate=False, data_sport=False, measure_sleep_timer=False):
        self.environment['BRIDGE_MODE'] = mode
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        identity = ['--device', 'AA:BB:CC:DD:EE:FF', '--expected-name', 'FS-TEST'] if identity_only else ['--profile', str(self.profile)]
        duration = ['--timeout', str(timeout)] if timeout is not None else []
        if zero_heart_rate:
            duration += ['--status-heart-rate-zero']
        if data_sport:
            duration += ['--status-data-sport']
        if measure_sleep_timer:
            duration += ['--measure-sleep-timer']
        process = subprocess.Popen([BINARY, 'status', '--backend', 'windows', *identity, *duration,
                                    '--confirm-diagnostics', '--address-type', 'public', '--log', str(self.log)],
                                   env=self.environment, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            os.write(master, confirmation)
            if stop:
                deadline = time.monotonic() + 5
                while not self.calls.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(self.calls.exists(), 'Helper never started')
                if stop == 'signal':
                    process.send_signal(signal.SIGTERM)
                else:
                    os.write(master, b'q')
            output, errors = process.communicate(timeout=8)
            self.assertEqual(termios.tcgetattr(slave), before)
            return process.returncode, output, errors
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_zero_heart_rate_query_is_explicit_and_reported(self):
        code, _, errors = self.probe(identity_only=True, zero_heart_rate=True)
        self.assertEqual(code, 0, errors)
        arguments = json.loads(self.calls.read_text())
        self.assertIn('-StatusHeartRateZero', arguments)
        self.assertNotIn('-ConfirmMotor', arguments)
        self.log.unlink()
        code, _, _ = self.probe(mode='variant-missing', identity_only=True, zero_heart_rate=True)
        self.assertEqual(code, 1)

    def test_zero_heart_rate_query_cannot_enter_other_paths(self):
        for arguments in [('scan', '--backend', 'windows'), ('status', '--backend', 'linux'),
                          ('control-test', '--backend', 'windows', '--dry-run')]:
            result = self.command(*arguments, '--status-heart-rate-zero')
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(self.calls.exists())

    def test_data_sport_query_is_explicit_and_nonmotor(self):
        code, _, errors = self.probe(identity_only=True, data_sport=True)
        self.assertEqual(code, 0, errors)
        arguments = json.loads(self.calls.read_text())
        self.assertIn('-StatusDataSport', arguments)
        self.assertNotIn('-ConfirmMotor', arguments)
        for args in [('scan', '--backend', 'windows'), ('status', '--backend', 'linux'),
                     ('control-test', '--backend', 'windows', '--dry-run'),
                     ('status', '--backend', 'windows', '--status-heart-rate-zero')]:
            result = self.command(*args, '--status-data-sport')
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_sleep_timer_is_explicit_and_nonmotor(self):
        code, _, errors = self.probe(identity_only=True, measure_sleep_timer=True)
        self.assertEqual(code, 0, errors)
        arguments = json.loads(self.calls.read_text())
        self.assertIn('-MeasureSleepTimer', arguments)
        self.assertNotIn('-ConfirmMotor', arguments)
        for args in [('scan', '--backend', 'windows'), ('status', '--backend', 'linux'),
                     ('control-test', '--backend', 'windows', '--dry-run'),
                     ('status', '--backend', 'windows', '--status-data-sport')]:
            result = self.command(*args, '--measure-sleep-timer')
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_sleep_timer_has_900_second_bound_only_in_timer_mode(self):
        code, _, errors = self.probe(identity_only=True, measure_sleep_timer=True, timeout=900)
        self.assertEqual(code, 0, errors)
        self.log.unlink()
        code, _, _ = self.probe(identity_only=True, measure_sleep_timer=True, timeout=901)
        self.assertEqual(code, 2)
        self.log.unlink(missing_ok=True)
        code, _, _ = self.probe(identity_only=True, timeout=900)
        self.assertEqual(code, 2)

    def test_long_nonmotor_observation_is_bounded_and_keeps_cleanup(self):
        code, _, errors = self.probe(identity_only=True, timeout=600)
        self.assertEqual(code, 0, errors)
        calls = json.loads(self.calls.read_text())
        self.assertEqual(calls[calls.index('-Timeout') + 1], '600')
        self.assertEqual(calls[calls.index('-Command') + 1], 'probe')
        self.assertNotIn('-ConfirmMotor', calls)
        self.log.unlink()
        self.calls.unlink()
        code, _, _ = self.probe(mode='wait', stop='key', identity_only=True, timeout=600)
        self.assertEqual(code, 1)
        self.log.unlink()
        self.calls.unlink()
        for arguments in (
            ['status', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF',
             '--expected-name', 'FS-TEST', '--confirm-diagnostics', '--timeout', '601'],
            ['status', '--profile', str(self.profile), '--confirm-diagnostics', '--timeout', '61'],
            ['scan', '--backend', 'windows', '--timeout', '61'],
            ['info', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF', '--timeout', '61'],
        ):
            result = self.command(*arguments)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(self.calls.exists())

    def test_scan_and_info_dispatch_and_address_type(self):
        for command in ('scan', 'info'):
            arguments = [command, '--backend', 'windows', '--timeout', '1']
            if command == 'info':
                arguments += ['--device', 'AA:BB:CC:DD:EE:FF', '--address-type', 'public']
            result = self.command(*arguments)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['command'], command)
            calls = json.loads(self.calls.read_text())
            self.assertIn('-Mta', calls)
            self.assertNotIn('-ConfirmDiagnostics', calls)
            if command == 'info':
                self.assertEqual(calls[calls.index('-Device') + 1], 'AA:BB:CC:DD:EE:FF')
                self.assertEqual(calls[calls.index('-AddressType') + 1], 'public')

    def test_invalid_options_never_launch_helper(self):
        for arguments in [
            ['scan', '--backend', 'guess'], ['scan', '--address-type', 'public'],
            ['scan', '--backend', 'windows', '--address-type', 'public'],
            ['info', '--backend', 'windows', '--device', 'invalid'],
            ['info', '--backend', 'windows', '--address-type', 'guess'],
            ['status', '--simulate', '--backend', 'windows'],
            ['workout', 'run', 'interval-demo', '--backend', 'windows'],
            ['status', '--backend', 'windows', '--profile', str(self.profile), '--confirm-diagnostics'],
            ['status', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF', '--confirm-diagnostics'],
            ['status', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF', '--expected-name', 'FS-TEST'],
            ['status', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF', '--expected-name', 'FS-TEST',
             '--profile', str(self.profile), '--confirm-diagnostics'],
            ['scan', '--backend', 'windows', '--device', 'AA:BB:CC:DD:EE:FF', '--expected-name', 'FS-TEST'],
        ]:
            with self.subTest(arguments=arguments):
                self.assertEqual(self.command(*arguments).returncode, 2)
                self.assertFalse(self.calls.exists())

    def test_probe_approval_and_arguments_are_preserved(self):
        code, output, errors = self.probe()
        self.assertEqual(code, 0, errors)
        self.assertFalse(json.loads(output)['motor_execution_enabled'])
        self.assertEqual(json.loads(self.log.read_text())['backend'], 'windows-winrt')
        calls = json.loads(self.calls.read_text())
        self.assertEqual(calls[calls.index('-ExpectedName') + 1], 'Name with spaces; $(never execute)')
        self.assertEqual(calls[calls.index('-Confirmation') + 1], 'NONMOTOR')
        self.assertIn('-Bridge', calls)

    def test_identity_only_probe_needs_no_calibration(self):
        code, _, errors = self.probe(identity_only=True)
        self.assertEqual(code, 0, errors)
        calls = json.loads(self.calls.read_text())
        self.assertEqual(calls[calls.index('-ExpectedName') + 1], 'FS-TEST')
        self.assertEqual(calls[calls.index('-Device') + 1], 'AA:BB:CC:DD:EE:FF')

    def test_decline_and_existing_log_do_not_launch_helper(self):
        code, _, _ = self.probe(confirmation=b'NO\n')
        self.assertEqual(code, 2)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.log.exists())
        self.log.write_text('preserve')
        code, _, _ = self.probe()
        self.assertEqual(code, 1)
        self.assertFalse(self.calls.exists())
        self.assertEqual(self.log.read_text(), 'preserve')

    def test_untrusted_or_malformed_child_report_is_rejected(self):
        for mode in ('motor', 'malformed'):
            self.environment['BRIDGE_MODE'] = mode
            self.assertEqual(self.command('scan', '--backend', 'windows', '--timeout', '1').returncode, 1)

    def test_unconfirmed_cleanup_cannot_succeed(self):
        code, _, _ = self.probe(mode='cleanup')
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(self.log.read_text())['notification_cleanup_status'], 'unconfirmed')

    def test_signal_and_key_request_graceful_cleanup(self):
        for stop in ('signal', 'key'):
            self.calls.unlink(missing_ok=True)
            self.log.unlink(missing_ok=True)
            code, output, errors = self.probe(mode='wait', stop=stop)
            self.assertEqual(code, 1, errors)
            self.assertEqual(json.loads(output)['notification_cleanup_status'], 'Success')


if __name__ == '__main__':
    unittest.main()
