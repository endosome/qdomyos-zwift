"""Offline summaries for treadmill JSON Lines logs; no Bluetooth access."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import summarize_run


class SummarizeRunTests(unittest.TestCase):
    def test_decodes_ftms_speed_and_inclination_using_flags(self):
        speed, incline = summarize_run.decode_treadmill_data(
            '8c05c8000a0000000000000000ffffff001200')
        self.assertEqual(speed, 2.0)
        self.assertEqual(incline, 0.0)

    def test_handles_more_data_and_short_frames(self):
        self.assertEqual(summarize_run.decode_treadmill_data('0100'), (None, None))
        self.assertEqual(summarize_run.decode_treadmill_data('8c05c8'), (None, None))

    def test_summarizes_status_transitions_and_stop_evidence(self):
        rows = [
            {'event': 'trial_plan', 'monotonic_ms': 0, 'trial': 'workout',
             'workout_name': 'short check', 'device': 'AA:BB', 'expected_name': 'FS-TEST'},
            {'event': 'raw_status', 'monotonic_ms': 1000, 'status_code': 3,
             'has_motor_data': True, 'speed_code': 20, 'incline_code': 0},
            {'event': 'raw_status', 'monotonic_ms': 1500, 'status_code': 3,
             'has_motor_data': True, 'speed_code': 20, 'incline_code': 0},
            {'event': 'ftms_notification', 'monotonic_ms': 1600,
             'characteristic_uuid': '00002acd-0000-1000-8000-00805f9b34fb',
             'frame_hex': '8c05c8000a0000000000000000ffffff001200'},
            {'event': 'raw_status', 'monotonic_ms': 2000, 'status_code': 4,
             'has_motor_data': True, 'speed_code': 18, 'incline_code': 0},
            {'event': 'stop_requested', 'monotonic_ms': 2100, 'reason': 'Unexpected motor feedback'},
            {'event': 'trial_result', 'monotonic_ms': 3000, 'sequence_observed': False,
             'stop_write_succeeded': True, 'normal_status_after_stop': True,
             'physical_stop_verified': False},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'run.jsonl'
            path.write_text('\n'.join(json.dumps(row) for row in rows) + '\n', encoding='utf-8')
            output = io.StringIO()
            with redirect_stdout(output):
                summarize_run.summarize(path)
        report = output.getvalue()
        self.assertIn('FitShow STOPPING, raw speed 18, incline 0', report)
        self.assertIn('FTMS reports speed 2.00 km/h, incline 0.0%', report)
        self.assertIn('stop_write_succeeded=True', report)
        self.assertIn('physical_stop_verified=False', report)
        self.assertEqual(report.count('FitShow RUNNING'), 1)

    def test_statistics_expose_step_overhead_and_unfinished_plans(self):
        steps = [{'index': i, 'hold_seconds': 10} for i in range(4)]
        rows = [{'event': 'trial_plan', 'monotonic_ms': 0, 'workout_steps': steps, 'windows_motor_deadline_seconds': 50}]
        for i, at in enumerate((1000, 12500, 24000)):  # 1.5 s overhead per step
            rows.append({'event': 'reported_target', 'monotonic_ms': at, 'stage': i})
        rows += [{'event': 'raw_status', 'monotonic_ms': 1000, 'status_code': 3},
                 {'event': 'raw_status', 'monotonic_ms': 1600, 'status_code': 3},
                 {'event': 'raw_status', 'monotonic_ms': 26000, 'status_code': 4},
                 {'event': 'stop_requested', 'monotonic_ms': 26100, 'reason': 'Missing status or start deadline'},
                 {'event': 'raw_status', 'monotonic_ms': 27000, 'status_code': 4}]
        stats = summarize_run.run_statistics(rows)
        self.assertEqual((stats['reported_targets'], stats['planned_targets']), (3, 4))
        mean, low, high = stats['step_overhead_s']
        self.assertAlmostEqual(mean, 1.5)
        self.assertAlmostEqual(stats['step_overhead_total_s'], 3.0)
        self.assertAlmostEqual(stats['max_status_gap_s'], 24.4)
        self.assertEqual(stats['stopping_before_stop_request_s'], [26.0])
        self.assertEqual(stats['stop_reason'], 'Missing status or start deadline')
        self.assertEqual(stats['nominal_hold_s'], 40)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'run.jsonl'
            path.write_text('\n'.join(json.dumps(row) for row in rows) + '\n', encoding='utf-8')
            output = io.StringIO()
            with redirect_stdout(output):
                summarize_run.summarize(path)
        self.assertIn('targets reported: 3 of 4', output.getvalue())
        self.assertIn('a console stop looks the same', output.getvalue())

    def test_elapsed_counts_from_the_start_and_reports_the_wait_separately(self):
        rows = [{'event': 'trial_plan', 'monotonic_ms': 0}, {'event': 'waiting_for_wake', 'monotonic_ms': 3000},
                {'event': 'start_requested', 'monotonic_ms': 185000}, {'event': 'raw_status', 'monotonic_ms': 190000, 'status_code': 3},
                {'event': 'raw_status', 'monotonic_ms': 285000, 'status_code': 3}]
        stats = summarize_run.run_statistics(rows)
        self.assertEqual(stats['elapsed_s'], 100.0)
        self.assertEqual(stats['wait_before_start_s'], 185.0)
        short = summarize_run.run_statistics([{'event': 'start_requested', 'monotonic_ms': 4000}, {'event': 'raw_status', 'monotonic_ms': 9000}])
        self.assertNotIn('wait_before_start_s', short)  # a normal few-second preflight is not a "wait"

    def test_statistics_without_a_plan_do_not_fail(self):
        stats = summarize_run.run_statistics([{'event': 'raw_status', 'monotonic_ms': 5, 'status_code': 0}])
        self.assertIsNone(stats['planned_targets'])
        self.assertEqual(stats['reported_targets'], 0)

    def test_reports_malformed_json_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'broken.jsonl'
            path.write_text('{not json}\n', encoding='utf-8')
            with self.assertRaises(ValueError) as error:
                summarize_run.read_json_lines(path)
            self.assertIn('invalid JSON', str(error.exception))


if __name__ == '__main__':
    unittest.main()
