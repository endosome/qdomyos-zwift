"""FIT workouts: build, round trip, W1 limits, corruption, FIT == XML in the CLI, and Garmin's decoder."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIT_TOOL = str(Path(sys.argv.pop(1)).resolve())
CLI = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
ADDRESS = ['--backend', 'linux', '--device', 'DD:87:70:39:74:20', '--expected-name', 'FS-0F7A32']

SPEC = {
    'id': 'unit', 'name': 'Unit training', 'description': 'Ramp, repeat, cooldown.', 'sport': 'hiking',
    'steps': [
        {'name': 'Warm-up', 'minutes': 3, 'speed': 2.4, 'level': 0, 'intensity': 'warmup', 'notes': 'Easy.'},
        {'ramp': {'name': 'Build', 'minutes': 4, 'speed_from': 2.4, 'speed_to': 3.6, 'level_from': 0, 'level_to': 4}},
        {'repeat': 3, 'steps': [
            {'name': 'Climb', 'minutes': 2, 'speed': 4.0, 'level': 6, 'intensity': 'interval'},
            {'name': 'Recover', 'minutes': 1, 'speed': 2.5, 'level': 1, 'intensity': 'recovery'}]},
        {'name': 'Cooldown', 'minutes': 3, 'speed': 2.0, 'level': 0, 'intensity': 'cooldown'}],
}


def run(*arguments):
    return subprocess.run(list(arguments), capture_output=True, text=True, timeout=60)


class FitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def spec(self, changes=None, name='unit'):
        spec = json.loads(json.dumps(SPEC))
        spec['id'] = name
        spec.update(changes or {})
        path = self.directory / f'{name}.json'
        path.write_text(json.dumps(spec))
        return path

    def build(self, spec_path, extra=()):
        out = spec_path.with_suffix('.fit')
        return run(FIT_TOOL, 'build', str(spec_path), str(out), *extra), out

    def refused(self, changes, message):
        result, out = self.build(self.spec(changes))
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(message, result.stderr)
        self.assertFalse(out.exists())

    def test_build_is_deterministic_and_check_detects_drift(self):
        spec = self.spec()
        first, out = self.build(spec)
        self.assertEqual(first.returncode, 0, first.stderr)
        data = out.read_bytes()
        self.assertEqual(run(FIT_TOOL, 'build', str(spec), str(out), '--overwrite').returncode, 0)
        self.assertEqual(out.read_bytes(), data)
        self.assertEqual(run(FIT_TOOL, 'check', str(spec), str(out)).returncode, 0)
        changed = self.spec({'name': 'Other name'})
        self.assertEqual(run(FIT_TOOL, 'check', str(changed), str(out)).returncode, 1)

    def test_existing_output_is_not_replaced_without_overwrite(self):
        spec = self.spec()
        _, out = self.build(spec)
        out.write_bytes(b'keep')
        self.assertEqual(self.build(spec)[0].returncode, 2)
        self.assertEqual(out.read_bytes(), b'keep')

    def test_dump_round_trips_ramps_repeats_and_totals(self):
        spec = self.spec()
        _, out = self.build(spec)[0], spec.with_suffix('.fit')
        dump = json.loads(run(FIT_TOOL, 'dump', str(out)).stdout)
        names = [step.get('name') or 'repeat x%d' % step['repeat'] for step in dump['steps']]
        self.assertEqual(names, ['Warm-up', 'Build 1/4', 'Build 2/4', 'Build 3/4', 'Build 4/4', 'repeat x3', 'Cooldown'])
        self.assertEqual(dump['total_seconds'], 180 + 240 + 3 * 180 + 180)
        self.assertEqual(dump['flat_steps'], 1 + 4 + 6 + 1)
        ramp = [s for s in dump['steps'] if s.get('name', '').startswith('Build')]
        self.assertEqual([(s['speed'], s['level']) for s in ramp], [(2.4, 0), (2.8, 1), (3.2, 3), (3.6, 4)])
        self.assertTrue(dump['steps'][5]['steps'][0]['notes'].startswith('L6 | 8.3% | 4.0 km/h | ~330 m/h'))
        self.assertEqual(dump['vertical_meters_estimate'], round(dump['vertical_meters_estimate']))

    def test_w1_limits_are_enforced_at_build_time(self):
        def with_step(**changes):
            step = {'name': 'S', 'minutes': 2, 'speed': 3.0, 'level': 2}
            step.update(changes)
            return {'steps': [step]}
        self.refused(with_step(speed=6.1), 'speed 6.1 must be 1-6')
        self.refused(with_step(speed=0.9), 'must be 1-6')
        self.refused(with_step(speed=3.05), '0.1 steps')
        self.refused(with_step(level=9), 'level 9 must be 0-8')
        self.refused(with_step(level=2.5), 'whole number')
        self.refused(with_step(seconds=10, minutes=None), "exactly one of 'minutes' or 'seconds'")
        self.refused({'steps': [{'name': 'S', 'seconds': 20, 'speed': 3.0, 'level': 2}]}, 'duration must be 30-3600')
        self.refused(with_step(intensity='flat-out'), 'unknown intensity')
        self.refused(with_step(name='x' * 33), 'name must be 1-32')
        self.refused(with_step(speeed=3.0), "unknown key 'speeed'")
        self.refused({'steps': [{'repeat': 1, 'steps': [{'name': 'S', 'minutes': 2, 'speed': 3.0, 'level': 2}]}]}, 'repeat must be a whole number from 2 to 40')
        self.refused({'sport': 'running'}, 'hiking or walking')
        self.refused({'name': ''}, 'name must be 1-40')
        self.refused({'steps': []}, "'steps' must be a non-empty array")
        self.refused({'extra': 1}, "unknown key 'extra'")

    def test_nested_repeats_and_over_long_trainings_are_refused(self):
        inner = {'repeat': 2, 'steps': [{'name': 'S', 'minutes': 2, 'speed': 3.0, 'level': 2}]}
        self.refused({'steps': [{'repeat': 2, 'steps': [inner]}]}, 'cannot be nested')
        long = {'repeat': 5, 'steps': [{'name': 'S', 'minutes': 15, 'speed': 3.0, 'level': 2}]}
        self.refused({'steps': [long]}, 'the W1 limit is 70')
        ok = {'repeat': 4, 'steps': [{'name': 'S', 'minutes': 17.5, 'speed': 3.0, 'level': 2}]}
        result, _ = self.build(self.spec({'steps': [ok]}, name='exactly70'))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ramp_rules(self):
        base = {'name': 'R', 'minutes': 4, 'speed_from': 2.0, 'speed_to': 3.0, 'level_from': 0, 'level_to': 4}
        self.refused({'steps': [{'ramp': dict(base, minutes=3, step_seconds=100)}]}, 'ramp needs at least two equal steps')
        self.refused({'steps': [{'ramp': dict(base, step_seconds=20)}]}, 'ramp needs at least two equal steps')
        self.refused({'steps': [{'ramp': dict(base, level_to=9)}]}, 'level 9 must be 0-8')
        self.refused({'steps': [{'ramp': dict(base), 'name': 'x'}]}, "'ramp' must be the only key")

    def test_file_name_must_match_the_spec_id(self):
        spec = self.spec()
        result = run(FIT_TOOL, 'build', str(spec), str(self.directory / 'different.fit'))
        self.assertEqual(result.returncode, 2)
        self.assertIn("file name must be 'unit.fit'", result.stderr)
        bad = self.directory / 'wrong-name.json'
        bad.write_text(spec.read_text())
        self.assertIn("'id' must equal the file name", self.build(bad)[0].stderr)

    def test_damaged_files_are_rejected_by_the_decoder(self):
        spec = self.spec()
        _, out = self.build(spec)[0], spec.with_suffix('.fit')
        good = out.read_bytes()
        flipped = bytearray(good)
        flipped[len(good) // 2] ^= 0x01
        cases = {'flipped': bytes(flipped), 'truncated': good[:-6], 'empty': b'', 'text': b'not a fit file at all'}
        for label, data in cases.items():
            with self.subTest(label):
                path = self.directory / f'{label}.fit'
                path.write_bytes(data)
                result = run(FIT_TOOL, 'dump', str(path))
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertEqual(result.stdout, '')
                if CLI:
                    self.assertNotEqual(run(CLI, 'workout', 'run', str(path), '--dry-run', *ADDRESS).returncode, 0)

    @unittest.skipUnless(CLI, 'needs the treadmill binary')
    def test_cli_plays_a_fit_workout_exactly_like_the_equivalent_xml(self):
        spec = self.spec()
        _, out = self.build(spec)[0], spec.with_suffix('.fit')
        dump = json.loads(run(FIT_TOOL, 'dump', str(out)).stdout)
        rows = []
        def row(step):
            rows.append('  <row name="%s" duration="%02d:%02d:%02d" speed="%s" inclination="%s" forcespeed="1"/>' % (
                step['name'], step['seconds'] // 3600, step['seconds'] % 3600 // 60, step['seconds'] % 60,
                step['speed'], round(step['level'] * 11.0 / 8.0, 3)))
        for node in dump['steps']:
            if 'repeat' in node:
                rows.append('  <repeat times="%d">' % node['repeat'])
                for step in node['steps']:
                    row(step)
                rows.append('  </repeat>')
            else:
                row(node)
        xml = self.directory / 'equivalent.xml'
        xml.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<rows version="1" id="equivalent" name="Unit training" '
                       'speedunit="km/h" inclineunit="percent" device="treadmill">\n' + '\n'.join(rows) + '\n</rows>\n')
        def plan(path):
            result = run(CLI, 'workout', 'run', str(path), '--dry-run', *ADDRESS)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            return data, [(s['hold_seconds'], s['speed_code'], s['incline_code']) for s in data['workout_steps']]
        fit_plan, fit_steps = plan(out)
        xml_plan, xml_steps = plan(xml)
        self.assertEqual(fit_steps, xml_steps)
        self.assertEqual(fit_plan['frames'], xml_plan['frames'])
        self.assertEqual(fit_plan['windows_motor_deadline_seconds'], xml_plan['windows_motor_deadline_seconds'])
        self.assertEqual((fit_plan['speed_ceiling_code'], fit_plan['incline_ceiling_code']), (40, 6))
        self.assertFalse(fit_plan['motor_execution_enabled'])

    @unittest.skipUnless(CLI, 'needs the treadmill binary')
    def test_workout_list_shows_fit_files_and_refuses_duplicate_ids(self):
        spec = self.spec()
        self.build(spec)
        listing = run(CLI, 'workout', 'list', '--workouts-dir', str(self.directory))
        self.assertEqual(listing.returncode, 0, listing.stderr)
        entry = json.loads(listing.stdout)[0]
        self.assertEqual((entry['id'], entry['name'], entry['duration_seconds']), ('unit', 'Unit training', 1140))
        (self.directory / 'copy.xml').write_text('<?xml version="1.0"?>\n<rows version="1" id="unit" name="Dup" speedunit="km/h" '
                                                 'inclineunit="percent" device="treadmill"><row name="a" duration="00:01:00" '
                                                 'speed="2" inclination="0" forcespeed="1"/></rows>')
        duplicate = run(CLI, 'workout', 'list', '--workouts-dir', str(self.directory))
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIn('Duplicate workout ID', duplicate.stderr)

    def test_garmin_official_decoder_reads_the_file_without_errors(self):
        try:
            from garmin_fit_sdk import Decoder, Stream
        except ImportError:
            self.skipTest("Garmin's garmin-fit-sdk is not installed")
        spec = self.spec()
        _, out = self.build(spec)[0], spec.with_suffix('.fit')
        # Check integrity on its own stream; the checks advance the stream position.
        self.assertTrue(Decoder(Stream.from_file(str(out))).is_fit())
        self.assertTrue(Decoder(Stream.from_file(str(out))).check_integrity())
        messages, errors = Decoder(Stream.from_file(str(out))).read()
        self.assertEqual(errors, [])
        self.assertEqual(messages['file_id_mesgs'][0]['type'], 'workout')
        self.assertEqual(messages['workout_mesgs'][0]['sport'], 'hiking')
        steps = messages['workout_step_mesgs']
        self.assertEqual(messages['workout_mesgs'][0]['num_valid_steps'], len(steps))
        first = steps[0]
        self.assertEqual((first['duration_type'], first['duration_time'], first['target_type']), ('time', 180.0, 'speed'))
        self.assertAlmostEqual(first['custom_target_speed_low'] * 3.6, 2.4, places=2)
        climb = steps[5]
        self.assertEqual((climb['secondary_target_type'], climb['secondary_custom_target_value_low']), ('grade', 825))
        repeat = steps[7]
        self.assertEqual((repeat['duration_type'], repeat['duration_step'], repeat['repeat_steps']),
                         ('repeat_until_steps_cmplt', 5, 3))
        self.assertEqual([step['message_index'] for step in steps], list(range(len(steps))))


if __name__ == '__main__':
    unittest.main()
