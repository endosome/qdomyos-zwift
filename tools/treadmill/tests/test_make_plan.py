"""Plan generator: output format, W1 limits and the CLI's own dry run."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import make_plan

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None


class MakePlanTests(unittest.TestCase):
    def generate(self, *arguments):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'plan.xml'
            code = make_plan.main(['--id', 'unit', '--out', str(path), *arguments])
            return code, path.read_text() if path.exists() else None

    def refused(self, *arguments):
        import contextlib, io
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            code, _ = self.generate(*arguments)
        self.assertEqual(code, 2)
        return errors.getvalue()

    def test_default_plan_is_a_sixty_minute_limit_safe_hike(self):
        code, xml = self.generate()
        self.assertEqual(code, 0)
        self.assertIn('<repeat times="3">', xml)
        self.assertIn('speed="3.0" inclination="5.5"', xml)      # level 4 = 4 x 1.375 %
        self.assertIn('inclination="1.375"', xml)                # level 1
        self.assertIn('id="unit"', xml)
        self.assertIn('inclinationfrom="1.375" inclinationto="0"', xml)

    def test_levels_map_to_the_working_grade_table(self):
        self.assertEqual([make_plan.grade(n) for n in range(9)],
                         ['0', '1.375', '2.75', '4.125', '5.5', '6.875', '8.25', '9.625', '11'])

    def test_limits_are_enforced_with_clear_messages(self):
        self.assertIn('1.0-6.0', self.refused('--climb-speed', '6.1'))
        self.assertIn('1.0-6.0', self.refused('--warmup-from', '0.9'))
        self.assertIn('0.1 steps', self.refused('--climb-speed', '3.05'))
        self.assertIn('level from 0 to 8', self.refused('--climb-level', '9'))
        self.assertIn('must not exceed', self.refused('--climb-level', '2', '--recover-level', '3'))
        self.assertIn('limit is 120', self.refused('--repeats', '20', '--climb-minutes', '5'))
        self.assertIn('at least 10', self.refused('--recover-minutes', '0.1'))
        self.assertIn('--id', make_plan_error(['--id', 'bad id']))

    def test_total_duration_adds_up(self):
        _, total = make_plan.build(make_args('--warmup', '5', '--repeats', '4', '--climb-minutes', '3',
                                             '--recover-minutes', '2', '--cooldown', '5'))
        self.assertEqual(total, (5 + 4 * 5 + 5) * 60)

    def test_existing_output_file_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'plan.xml'
            path.write_text('keep')
            import contextlib, io
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(make_plan.main(['--id', 'unit', '--out', str(path)]), 2)
            self.assertEqual(path.read_text(), 'keep')

    @unittest.skipUnless(BINARY, 'needs the treadmill binary')
    def test_generated_plan_passes_the_cli_dry_run_with_matching_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'hike.xml'
            self.assertEqual(make_plan.main(['--id', 'hike', '--out', str(path), '--repeats', '5',
                                             '--climb-level', '6', '--climb-speed', '3.5']), 0)
            plan = make_plan.check(BINARY, path)
        self.assertEqual(plan['workout_id'], 'hike')
        self.assertEqual(plan['speed_ceiling_code'], 35)
        self.assertEqual(plan['incline_ceiling_code'], 6)
        nominal = (5 + 5 * (4 + 2) + 5) * 60          # warm-up + 5 x (climb + recover) + cooldown = 40 min
        self.assertGreater(plan['windows_motor_deadline_seconds'], nominal + 120)
        self.assertFalse(plan['motor_execution_enabled'])


def make_args(*arguments):
    import argparse
    captured = {}
    original = make_plan.build
    def capture(args):
        captured['args'] = args
        return original(args)
    make_plan.build = capture
    try:
        import contextlib, io
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            make_plan.main(['--id', 'unit', '--out', str(Path(directory) / 'x.xml'), *arguments])
    finally:
        make_plan.build = original
    return captured['args']


def make_plan_error(arguments):
    import contextlib, io
    with contextlib.redirect_stderr(io.StringIO()) as errors:
        make_plan.main(arguments)
    return errors.getvalue()


if __name__ == '__main__':
    unittest.main()
