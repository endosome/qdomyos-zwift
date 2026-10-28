"""The TUI's model of a run, built from the CLI's JSON records; no terminal, Bluetooth or CLI needed.
The pty driver is exercised against the real CLI in test_motortrial.py."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import treadmill_tui as tui  # noqa: E402

STEPS = [dict(index=0, name='Warm-up 1/2', hold_seconds=10, speed_code=20, incline_code=0),
         dict(index=1, name='Warm-up 2/2', hold_seconds=10, speed_code=25, incline_code=0),
         dict(index=2, name='Climb', hold_seconds=20, speed_code=35, incline_code=4),
         dict(index=3, name='Recover', hold_seconds=15, speed_code=30, incline_code=1),
         dict(index=4, name='Climb', hold_seconds=20, speed_code=35, incline_code=4)]


def state_with_plan():
    state = tui.RunState()
    state.feed(dict(event='trial_plan', monotonic_ms=0, workout_steps=STEPS, workout_name='Demo', profile='walking',
                    console_pause_allowance_seconds=900))
    return state


class SegmentTests(unittest.TestCase):
    def test_ramp_pieces_share_a_segment_and_repeats_are_numbered(self):
        self.assertEqual(tui.base_name('Warm-up 3/6'), 'Warm-up')
        self.assertEqual(tui.base_name('Brisk 30 s'), 'Brisk 30 s')
        self.assertEqual(tui.base_name('Steep 1/x'), 'Steep 1/x')
        groups = tui.segments(STEPS)
        self.assertEqual([(g['name'], g['first'], g['last'], g['seconds'], g['number'], g['of']) for g in groups],
                         [('Warm-up', 0, 1, 20, 1, 1), ('Climb', 2, 2, 20, 1, 2), ('Recover', 3, 3, 15, 1, 1), ('Climb', 4, 4, 20, 2, 2)])


class PlanClockTests(unittest.TestCase):
    def test_the_step_clock_starts_at_the_reported_target_and_freezes_through_a_console_pause(self):
        state = state_with_plan()
        state.feed(dict(event='reported_running', monotonic_ms=1000))
        state.feed(dict(event='target_requested', monotonic_ms=1000, stage=0, speed_code=20, incline_code=0))
        self.assertEqual(state.step_left_ms(1500), 10000)  # not reported yet: the hold has not started
        state.feed(dict(event='reported_target', monotonic_ms=2000, stage=0))
        self.assertEqual(state.step_left_ms(7000), 5000)
        self.assertEqual(state.segment_left_ms(7000), 15000)
        state.feed(dict(event='console_pause', monotonic_ms=7000, status_code=4))
        self.assertEqual(state.phase, 'paused')
        self.assertEqual(state.step_left_ms(20000), 5000)
        self.assertAlmostEqual(state.pause_allowance_left_s(20000), 900 - 13)
        state.feed(dict(event='console_play', monotonic_ms=20000))
        state.feed(dict(event='resume_target_requested', monotonic_ms=20000, speed_code=15, incline_code=0))
        self.assertEqual((state.phase, state.target_speed), ('resuming', 15))
        self.assertEqual(state.step_left_ms(22000), 5000)  # the ramp back does not count against the step
        state.feed(dict(event='console_resumed', monotonic_ms=23000, speed_code=20, incline_code=0))
        self.assertEqual(state.phase, 'running')
        self.assertEqual(state.step_left_ms(24000), 4000)
        self.assertEqual((state.pauses, state.idle_ms), (1, 13000))
        state.feed(dict(event='target_requested', monotonic_ms=28000, stage=1, speed_code=25, incline_code=0))
        self.assertEqual(state.plan_elapsed_ms(28000), 10000)
        self.assertEqual(state.plan_total_ms(), 75000)

    def test_a_pause_before_the_step_is_reached_starts_its_hold_when_the_run_is_back_on_the_plan(self):
        state = state_with_plan()
        state.feed(dict(event='reported_running', monotonic_ms=1000))
        state.feed(dict(event='target_requested', monotonic_ms=1000, stage=0, speed_code=20, incline_code=0))
        state.feed(dict(event='console_pause', monotonic_ms=1500, status_code=4))
        state.feed(dict(event='console_play', monotonic_ms=5000))
        state.feed(dict(event='console_resumed', monotonic_ms=7000))
        self.assertEqual(state.step_left_ms(9000), 8000)

    def test_a_refused_pause_marks_pause_as_unsupported(self):
        state = state_with_plan()
        state.feed(dict(event='manual_command_result', monotonic_ms=5, seq=1, cmd='pause', result='refused', reason='unknown command'))
        self.assertIs(state.pause_supported, False)
        self.assertEqual(state.phase, 'connecting')
        self.assertIn('console', state.messages[-1][2])

    def test_an_applied_host_pause_freezes_the_plan_like_a_console_pause(self):
        state = state_with_plan()
        state.feed(dict(event='reported_running', monotonic_ms=1000))
        state.feed(dict(event='reported_target', monotonic_ms=1000, stage=0))
        state.feed(dict(event='manual_command_result', monotonic_ms=4000, seq=1, cmd='pause', result='applied'))
        self.assertEqual((state.phase, state.step_left_ms(9000)), ('paused', 7000))
        state.feed(dict(event='manual_command_result', monotonic_ms=9000, seq=2, cmd='resume', result='applied'))
        state.feed(dict(event='console_resumed', monotonic_ms=10000))
        self.assertEqual((state.phase, state.step_left_ms(11000)), ('running', 6000))

    def test_applied_and_refused_manual_commands(self):
        state = state_with_plan()
        state.feed(dict(event='manual_command_result', monotonic_ms=5, seq=1, cmd='speed_up', result='applied',
                        speed_code=21, incline_code=0, stage=0))
        self.assertEqual((state.target_speed, state.target_incline), (21, 0))
        state.feed(dict(event='manual_command_result', monotonic_ms=6, seq=2, cmd='incline_up', result='refused',
                        reason='at the limit', speed_code=21, incline_code=8))
        self.assertEqual(state.target_incline, 0)
        self.assertIn('at the limit', state.messages[-1][2])


class SafetyAndDistanceTests(unittest.TestCase):
    def test_a_removed_safety_key_is_reported_once_with_its_time(self):
        state = state_with_plan()
        state.feed(dict(event='reported_running', monotonic_ms=1000))
        state.feed(dict(event='raw_status', monotonic_ms=5000, utc='2026-10-28T10:00:00.000Z', status_code=6, disabled_reason_code=1,
                        has_motor_data=False))
        state.feed(dict(event='raw_status', monotonic_ms=5300, utc='2026-10-28T10:00:00.300Z', status_code=6, disabled_reason_code=1,
                        has_motor_data=False))
        self.assertEqual(state.key_removed, (5000, '2026-10-28T10:00:00.000Z'))
        self.assertEqual(sum('SAFETY KEY' in text for _, _, text in state.messages), 1)
        sleeping = state_with_plan()
        sleeping.feed(dict(event='raw_status', monotonic_ms=10, status_code=6, disabled_reason_code=2, has_motor_data=False))
        self.assertIsNone(sleeping.key_removed)  # 06/02 is sleep, not the key

    def test_distance_is_integrated_from_speed_until_the_treadmill_reports_its_own(self):
        state = state_with_plan()
        state.feed(dict(event='reported_running', monotonic_ms=0))
        for second in range(11):  # 3.6 km/h = 1 m/s at level 4 (5.5 % estimated grade)
            state.feed(dict(event='raw_status', monotonic_ms=second * 1000, status_code=3, has_motor_data=True, speed_code=36, incline_code=4))
        self.assertEqual(state.distance(), (10.0, 'estimate'))
        self.assertAlmostEqual(state.climb_m, 0.55)
        state.feed(dict(event='raw_status', monotonic_ms=12000, status_code=4, has_motor_data=True, speed_code=20, incline_code=4))
        state.feed(dict(event='raw_status', monotonic_ms=13000, status_code=0, has_motor_data=True, speed_code=0, incline_code=0))
        self.assertEqual(state.distance()[0], 12.0)  # the interval up to the first non-RUNNING report counts, nothing after
        frame = lambda metres: '8c05' + '0000' + metres.to_bytes(3, 'little').hex() + '00' * 12
        for metres in (5, 40, 3, 9):  # the counter restarts after a console pause
            state.feed(dict(event='ftms_notification', monotonic_ms=14000, characteristic_uuid=tui.FTMS_TREADMILL_DATA, frame_hex=frame(metres)))
        self.assertEqual(state.distance(), (35 + 9, 'treadmill'))
        state.feed(dict(event='distance_reported', monotonic_ms=15000, distance_m=50.0))
        self.assertEqual(state.distance(), (50.0, 'treadmill'))


class StderrTests(unittest.TestCase):
    def test_the_plan_json_is_dropped_and_prompts_without_a_newline_are_counted(self):
        parser = tui.StderrParser()
        shown = parser.feed('{\n    "trial": "workout",\n    "frames": [\n    ]\n}\nThis runs the prepared W1 target schedule.\nPress ENTER to app')
        self.assertEqual((shown, parser.prompts), (['This runs the prepared W1 target schedule.'], 0))
        self.assertEqual(parser.feed('rove this exact trial (anything else cancels): '), [])
        self.assertEqual(parser.prompts, 1)
        self.assertEqual(parser.feed('Press ENTER again to confirm: Treadmill is asleep. Wake it.\n'), ['Treadmill is asleep. Wake it.'])
        self.assertEqual(parser.prompts, 2)


if __name__ == '__main__':
    unittest.main()
