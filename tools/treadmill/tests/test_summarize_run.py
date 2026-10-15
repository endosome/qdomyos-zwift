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

    def test_reports_malformed_json_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'broken.jsonl'
            path.write_text('{not json}\n', encoding='utf-8')
            with self.assertRaises(ValueError) as error:
                summarize_run.read_json_lines(path)
            self.assertIn('invalid JSON', str(error.exception))


if __name__ == '__main__':
    unittest.main()
