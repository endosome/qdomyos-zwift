"""Linux helper argument validation; no Bluetooth access is attempted."""
import subprocess
import sys
import unittest
from pathlib import Path

HELPER = str(Path(sys.argv.pop(1)).resolve())
BASE = ['--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32', '--confirm-motor']


def run(*arguments, stdin=''):
    return subprocess.run([HELPER, *arguments], input=stdin, capture_output=True, text=True, timeout=10)


class LinuxHelperTests(unittest.TestCase):
    def refused(self, *arguments, message):
        result = run(*arguments)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertEqual(result.stdout, '')

    def test_requires_the_host_confirmation_flag(self):
        self.refused('--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32',
                     message='--confirm-motor')

    def test_requires_a_valid_address_and_exact_name(self):
        self.refused('--confirm-motor', '--expected-name', 'FS-0F7A32', message='--device and --expected-name')
        self.refused('--confirm-motor', '--device', 'not-an-address', '--expected-name', 'FS-0F7A32',
                     message='--device and --expected-name')
        self.refused('--confirm-motor', '--device', 'DD:87:70:39:74:20', '--expected-name', '  ',
                     message='--device and --expected-name')

    def test_only_reviewed_trials_are_available(self):
        for trial in ('pause', 'ready-wake', 'incline-timing-8', 'speed-70', 'incline-9', 'anything'):
            with self.subTest(trial=trial):
                self.refused(*BASE, '--trial', trial, message='not available on the Linux backend')

    def test_workout_needs_a_valid_target_sequence(self):
        self.refused(*BASE, '--trial', 'workout', '--timeout', '600', message='outside the reviewed limit')
        for sequence in ('9:0', '61:0', '20:9', '20:1,20:1', 'x', '20'):
            with self.subTest(sequence=sequence):
                self.refused(*BASE, '--trial', 'workout', '--timeout', '600', '--target-sequence', sequence,
                             message='Workout targets must be distinct')

    def test_deadline_is_bounded(self):
        self.refused(*BASE, '--timeout', '0', message='Session duration')
        self.refused(*BASE, '--timeout', '241', message='Session duration')
        self.refused(*BASE, '--trial', 'workout', '--target-sequence', '20:1', '--timeout', '4501',
                     message='Session duration')
        self.refused(*BASE, '--timeout', 'soon', message='Session duration')


if __name__ == '__main__':
    unittest.main()
