"""Installed-image checks without network, host mounts, Bluetooth or source tree."""
import json
import subprocess
import sys
import unittest

IMAGE = sys.argv.pop(1)


class PackageTests(unittest.TestCase):
    def command(self, *arguments, shell=False):
        command = ['docker', 'run', '--rm', '--network', 'none', '--read-only',
                   '--tmpfs', '/tmp']
        if shell:
            command += ['--entrypoint', '/bin/sh']
        return subprocess.run([*command, IMAGE, *arguments], capture_output=True,
                              text=True, timeout=30)

    def test_installed_catalog(self):
        result = self.command('workout', 'list')
        self.assertEqual(result.returncode, 0, result.stderr)
        catalog = json.loads(result.stdout)
        core = {'uphill-walk-20': 1200, 'uphill-walk-60': 3600, 'interval-demo': 120, 'runpad-w1-discovery-demo': 310}
        self.assertEqual({row['id']: row['duration_seconds'] for row in catalog if row['id'] in core}, core)
        for row in catalog:
            if row['id'] not in core:
                self.assertTrue(row['path'].endswith('.fit'), row)
        for row in catalog:
            self.assertTrue(row['path'].startswith('/usr/local/share/treadmill/workouts/'))

    def test_complete_installed_workouts(self):
        for plan, duration in [('uphill-walk-20', 1200000), ('interval-demo', 120000)]:
            with self.subTest(plan=plan):
                # The plan comes from the fixed test table, passed as a shell argument.
                result = self.command('-c',
                    'treadmill workout run "$1" --simulate --fast --log /tmp/session.jsonl '
                    '> /tmp/output; result=$?; cat /tmp/session.jsonl; exit "$result"',
                    'package-test', plan, shell=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                final = json.loads(result.stdout.splitlines()[-1])
                self.assertEqual(final['state'], 'COMPLETED')
                self.assertEqual(final['elapsed_ms'], duration)
                self.assertEqual(final['reported']['speed_kmh'], 0)

    def test_installed_profile_and_hardware_gate(self):
        profile = '/usr/local/share/treadmill/profiles/unverified-example.json'
        result = self.command('workout', 'inspect', 'uphill-walk-20', '--profile', profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)['motor_execution_enabled'])
        result = self.command('workout', 'run', 'uphill-walk-20', '--profile', profile)
        self.assertEqual(result.returncode, 2)
        self.assertIn('disabled', result.stderr)

    def test_runtime_and_corresponding_source(self):
        result = self.command('-c',
            'test ! -e /source && test ! -e /build && test "$(id -u)" = 65534 '
            '&& test -s /usr/local/share/doc/treadmill/LICENSE '
            '&& test -s /usr/local/share/doc/treadmill/API.md '
            '&& test -s /usr/local/share/doc/treadmill/build-packages.txt '
            '&& tar -tzf /usr/local/share/doc/treadmill/corresponding-source.tar.gz',
            shell=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        files = result.stdout.splitlines()
        self.assertIn('./tools/treadmill/treadmill.pro', files)
        self.assertIn('./src/devices/fitshowtreadmill/fitshowcontrol.cpp', files)
        self.assertIn('./LICENSE', files)
        self.assertNotIn('./src/main.cpp', files)
        self.assertNotIn('./src/fit-sdk/fit_mesg.cpp', files)


if __name__ == '__main__':
    unittest.main()
