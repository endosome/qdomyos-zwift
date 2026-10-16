"""The training library and plans: specs match the FIT files, W1 limits, plan guard-rails, docs in sync."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIT_TOOL = str(Path(sys.argv.pop(1)).resolve())
CLI = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
ROOT = Path(__file__).resolve().parents[1]
SPECS = sorted((ROOT / 'trainings').glob('*.json'))
PLANS = sorted((ROOT / 'plans').glob('*.json'))
ADDRESS = ['--backend', 'linux', '--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32']
sys.path.insert(0, str(ROOT))
import plan as plan_tool


def run(*arguments, **kwargs):
    return subprocess.run([str(a) for a in arguments], capture_output=True, text=True, timeout=120, **kwargs)


def dump(fit):
    result = run(FIT_TOOL, 'dump', fit)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


class TrainingLibraryTests(unittest.TestCase):
    def test_there_are_trainings_for_each_discipline(self):
        ids = [path.stem for path in SPECS]
        self.assertGreaterEqual(len(ids), 15)
        for prefix in ('skimo-', 'hike-', 'mr-', 'mrun-', 'trail-'):
            self.assertGreaterEqual(sum(i.startswith(prefix) for i in ids), 3, prefix)

    def test_every_fit_file_is_exactly_what_its_spec_builds(self):
        result = run(ROOT / 'trainings' / 'build.sh', '--check', env=dict(os.environ, TREADMILL_FIT=FIT_TOOL))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_orphan_fit_files_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            sandbox = Path(directory)
            (sandbox / 'trainings').mkdir()
            (sandbox / 'workouts').mkdir()
            script = sandbox / 'trainings' / 'build.sh'
            script.write_text((ROOT / 'trainings' / 'build.sh').read_text())
            script.chmod(0o755)
            first = sorted((ROOT / 'trainings').glob('*.json'))[0]
            (sandbox / 'trainings' / first.name).write_text(first.read_text())
            (sandbox / 'workouts' / (first.stem + '.fit')).write_bytes((ROOT / 'workouts' / (first.stem + '.fit')).read_bytes())
            (sandbox / 'workouts' / 'stray.fit').write_bytes(b'x')
            result = run(script, '--check', env=dict(os.environ, TREADMILL_FIT=FIT_TOOL))
            self.assertEqual(result.returncode, 1)
            self.assertIn('no spec for', result.stderr)

    def test_trainings_stay_inside_the_w1_and_the_library_limits(self):
        names = set()
        for spec in SPECS:
            with self.subTest(spec=spec.stem):
                info = dump(ROOT / 'workouts' / (spec.stem + '.fit'))
                self.assertEqual(info['id'], spec.stem)
                self.assertTrue(20 * 60 <= info['total_seconds'] <= 120 * 60, info['total_seconds'])
                if info['total_seconds'] > 65 * 60:  # nothing past 62 minutes has run on this treadmill
                    self.assertIn('UNPROVEN LENGTH', info['description'])
                steps = list(plan_tool.flat_steps(info['steps']))
                running = info['sport'] == 'running'
                self.assertLessEqual(max(s['speed'] for s in steps), 12.0 if running else 6.0)
                self.assertGreaterEqual(min(s['speed'] for s in steps), 1.0)
                self.assertLessEqual(max(s['level'] for s in steps), 8)
                self.assertTrue(all(s['seconds'] >= 30 for s in steps))
                self.assertIn(info['sport'], ('hiking', 'running'))
                self.assertEqual(info.get('sub_sport'), 'trail' if running else None)
                if running:
                    self.assertGreater(max(s['speed'] for s in steps), 6.0, 'a running training should use running speeds')
                    self.assertIn('--running-mode', info['description'])
                self.assertTrue(info['description'])
                self.assertNotIn(info['name'], names)
                names.add(info['name'])
                self.assertEqual(json.loads(spec.read_text())['id'], spec.stem)

    @unittest.skipUnless(CLI, 'needs the treadmill binary')
    def test_the_cli_accepts_every_training_with_matching_ceilings(self):
        for spec in SPECS:
            with self.subTest(spec=spec.stem):
                fit = ROOT / 'workouts' / (spec.stem + '.fit')
                info = dump(fit)
                steps = list(plan_tool.flat_steps(info['steps']))
                running = info['sport'] == 'running'
                with tempfile.TemporaryDirectory() as state:
                    env = dict(os.environ, TREADMILL_STATE_DIR=state)
                    if running:
                        # Without the flag, then with the flag but nothing validated: both refuse. Only then validate 12 km/h.
                        self.assertIn('add --running-mode', run(CLI, 'workout', 'run', fit, '--dry-run', *ADDRESS, env=env).stderr)
                        self.assertIn('validated only up to 6 km/h', run(CLI, 'workout', 'run', fit, '--dry-run', *ADDRESS, '--running-mode', env=env).stderr)
                        Path(state, 'running-validated.json').write_text('{"validated_kmh": 12}')
                    result = run(CLI, 'workout', 'run', fit, '--dry-run', *ADDRESS, *(['--running-mode'] if running else []), env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                plan = json.loads(result.stdout)
                self.assertEqual(plan['workout_id'], spec.stem)
                self.assertEqual(plan['speed_ceiling_code'], round(max(s['speed'] for s in steps) * 10))
                self.assertEqual(plan['incline_ceiling_code'], max(s['level'] for s in steps))
                self.assertLessEqual(plan['step_count'], 512)
                # The library's normal sessions also fit the Windows bridge's 70 minute / 4500 s limit; the long ones are Linux only.
                self.assertLessEqual(plan['windows_motor_deadline_seconds'], 4500 if info['total_seconds'] <= 65 * 60 else 9000)
                self.assertEqual(plan['profile'], 'running' if running else 'walking')
                self.assertFalse(plan['motor_execution_enabled'])

    def test_garmin_official_decoder_reads_every_training_cleanly(self):
        try:
            from garmin_fit_sdk import Decoder, Stream
        except ImportError:
            self.skipTest("Garmin's garmin-fit-sdk is not installed")
        for spec in SPECS:
            with self.subTest(spec=spec.stem):
                path = str(ROOT / 'workouts' / (spec.stem + '.fit'))
                self.assertTrue(Decoder(Stream.from_file(path)).check_integrity())
                messages, errors = Decoder(Stream.from_file(path)).read()
                self.assertEqual(errors, [])
                steps = messages['workout_step_mesgs']
                self.assertEqual(messages['workout_mesgs'][0]['num_valid_steps'], len(steps))
                self.assertEqual([s['message_index'] for s in steps], list(range(len(steps))))

    def test_readme_catalog_matches_the_fit_files(self):
        text = (ROOT / 'trainings' / 'README.md').read_text()
        start, end = '<!-- catalog:start -->', '<!-- catalog:end -->'
        self.assertIn(start, text)
        block = text.split(start, 1)[1].split(end, 1)[0].strip()
        expected = run(sys.executable, ROOT / 'plan.py', 'catalog', '--fit-tool', FIT_TOOL)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        self.assertEqual(block, expected.stdout.strip())


class PlanTests(unittest.TestCase):
    def check(self, plan):
        return plan_tool.check(plan, plan_tool.Trainings(ROOT / 'workouts', FIT_TOOL), 'test')

    def good_week(self, n, sessions=None):
        return {'week': n, 'focus': 'f', 'sessions': sessions or [
            {'day': 'Tue', 'training': 'hike-base-60'}, {'day': 'Thu', 'training': 'hike-recovery-35'}, {'day': 'Sat', 'training': 'hike-base-60'}]}

    def plan(self, weeks):
        return {'id': 'p', 'name': 'P', 'goal': 'G', 'weeks': weeks}

    def test_shipped_plans_pass_and_every_training_is_used(self):
        used = set()
        for path in PLANS:
            with self.subTest(plan=path.name):
                plan = json.loads(path.read_text())
                self.assertEqual(self.check(plan), [])
                self.assertEqual(plan['id'], path.stem)
                for week in plan['weeks']:
                    used |= {s['training'] for s in week['sessions'] if 'training' in s}
        unproven = {p.stem for p in SPECS if 'UNPROVEN LENGTH' in json.loads(p.read_text())['description']}
        self.assertGreaterEqual(len(unproven), 3)
        self.assertEqual(sorted(set(p.stem for p in SPECS) - used - unproven), [])
        self.assertEqual(sorted(used & unproven), [])

    def test_plans_show_without_errors(self):
        for path in PLANS:
            result = run(sys.executable, ROOT / 'plan.py', 'show', path, '--fit-tool', FIT_TOOL)
            self.assertEqual(result.returncode, 0, result.stderr)
            commands = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith('treadmill workout run')]
            self.assertTrue(commands)
            plan = json.loads(path.read_text())
            running = {s['training'] for w in plan['weeks'] for s in w['sessions'] if 'training' in s
                       and json.loads((ROOT / 'trainings' / (s['training'] + '.json')).read_text())['sport'] == 'running'}
            for command in commands:
                training = command.split()[3]
                self.assertEqual('--running-mode' in command, training in running, command)
                self.assertIn('--wait-wake 10 --wait-wake-confirm', command)

    def test_guard_rails_reject_unsafe_or_malformed_plans(self):
        weeks = lambda *w: self.plan(list(w))
        base = [self.good_week(n) for n in range(1, 5)]
        self.assertEqual(self.check(self.plan(base)), [])
        cases = {
            'at least 4 weeks': self.plan(base[:3]),
            "unknown training 'nope'": self.plan([self.good_week(1, [{'day': 'Tue', 'training': 'nope'}])] + base[1:]),
            'hard sessions on consecutive days': self.plan([self.good_week(1, [
                {'day': 'Tue', 'training': 'mr-hill-repeats-8x2'}, {'day': 'Wed', 'training': 'skimo-threshold-4x8'}])] + base[1:]),
            'hard sessions on consecutive days (week 1 Sun and Mon)': self.plan([self.good_week(1, [
                {'day': 'Tue', 'training': 'hike-base-60'}, {'day': 'Sun', 'training': 'mr-hill-repeats-8x2'}]),
                self.good_week(2, [{'day': 'Mon', 'training': 'skimo-threshold-4x8'}])] + base[2:]),
            '3 hard sessions': self.plan([self.good_week(1, [
                {'day': 'Mon', 'training': 'mr-hill-repeats-8x2'}, {'day': 'Wed', 'training': 'skimo-threshold-4x8'},
                {'day': 'Fri', 'training': 'mr-powerhike-5x5'}])] + base[1:]),
            'days must be in order and unique': self.plan([self.good_week(1, [
                {'day': 'Tue', 'training': 'hike-base-60'}, {'day': 'Tue', 'training': 'hike-recovery-35'}])] + base[1:]),
            "exactly one of 'training' or 'off'": self.plan([self.good_week(1, [
                {'day': 'Tue', 'training': 'hike-base-60', 'off': 'rest'}])] + base[1:]),
            "unknown session key 'nots'": self.plan([self.good_week(1, [{'day': 'Tue', 'training': 'hike-base-60', 'nots': 'x'}])] + base[1:]),
            'more than 5 training days': self.plan([self.good_week(1, [
                {'day': d, 'training': 'hike-recovery-35'} for d in ('Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat')])] + base[1:]),
            'more than 20% above': self.plan([self.good_week(1, [{'day': 'Tue', 'training': 'hike-recovery-35'}]),
                                              self.good_week(2, [{'day': 'Tue', 'training': 'hike-base-60'}, {'day': 'Thu', 'training': 'hike-base-60'}])] + base[2:]),
            'taper': self.plan(base[:3] + [self.good_week(4, [{'day': 'Tue', 'training': 'hike-base-60'}, {'day': 'Thu', 'training': 'hike-base-60'},
                                                              {'day': 'Sat', 'training': 'hike-base-60'}, {'day': 'Sun', 'training': 'hike-base-60'}])]),
            "'week' must be 2": self.plan([base[0], dict(base[1], week=3), base[2], base[3]]),
            "unknown key 'extra'": dict(self.plan(base), extra=1),
            "missing 'goal'": {'id': 'p', 'name': 'P', 'weeks': base},
            'has an unproven length': self.plan([self.good_week(1, [{'day': 'Tue', 'training': 'hike-long-day-100'}])] + base[1:]),
        }
        for message, plan in cases.items():
            with self.subTest(message):
                errors = self.check(plan)
                self.assertTrue(any(message in error for error in errors), errors)

    def test_load_limit_compares_with_the_larger_of_the_previous_two_weeks(self):
        light = self.good_week(3, [{'day': 'Tue', 'training': 'hike-recovery-35'}])
        # 155, 155, 35 (down week), 155 again is allowed: 155 <= 1.2 x max(155, 35). The last week tapers.
        final = self.good_week(5, [{'day': 'Tue', 'training': 'hike-recovery-35'}])
        self.assertEqual(self.check(self.plan([self.good_week(1), self.good_week(2), dict(light), self.good_week(4), final])), [])


if __name__ == '__main__':
    unittest.main()
