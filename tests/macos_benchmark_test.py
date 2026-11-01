import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from macos.benchmark import evaluate


def capture():
    # 160 Hz synthetic timestamps, enough headroom to avoid floating point ties.
    return [dict(ticket=i+1, frame=i+1, kind='anchor', anchor_id=i+1, phase=1.0,
                 state_generation=i+1, present_seconds=1+i/160, strict=1,
                 scene_width=3840, scene_height=2160, output_width=3840, output_height=2160,
                 drawable_width=3840, drawable_height=2160) for i in range(321)]


def phase_capture():
    phases = (1 / 3, 2 / 3, 1.0)
    return [dict(ticket=i+1, frame=i//3+1, kind='anchor' if i%3 == 2 else 'phase',
                 anchor_id=i//3+1, phase=phases[i%3], state_generation=i+1,
                 present_seconds=1+i/160, strict=1, scene_width=3840, scene_height=2160,
                 output_width=3840, output_height=2160, drawable_width=3840, drawable_height=2160)
            for i in range(321)]


class PresentationTests(unittest.TestCase):
    def test_native_rendered_frames(self):
        self.assertEqual(evaluate(capture(), minimum_seconds=1)['status'], 'pass')

    def test_native_phase_states_are_full_frames(self):
        self.assertEqual(evaluate(phase_capture(), minimum_seconds=1)['status'], 'pass')

    def test_exact_144_hz_survives_csv_timestamp_rounding(self):
        rows=capture()
        for i, row in enumerate(rows): row['present_seconds']=f'{20000+i/144:.9f}'
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'pass', result)
        self.assertEqual(result['over_budget'], 0)
        self.assertLess(result['timestamp_precision_ns'], 2)

    def test_callback_order_does_not_change_display_order(self):
        rows=capture(); rows[5],rows[6]=rows[6],rows[5]
        self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'pass')
        rows[5]['present_seconds'], rows[6]['present_seconds'] = rows[6]['present_seconds'], rows[5]['present_seconds']
        self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'fail')

    def test_rejects_false_performance_claims(self):
        for key, value in [('scene_width', 2560), ('scene_height', 0), ('drawable_width', 2560),
                           ('strict', 0), ('frame', 1), ('state_generation', 1), ('ticket', 1000),
                           ('kind', 'interpolated'), ('phase', 0.5)]:
            with self.subTest(key=key):
                rows=capture(); rows[5][key]=value
                self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'fail')

    def test_rejects_repeated_or_reordered_phase_states(self):
        for key, value in [('state_generation', 5), ('anchor_id', 1), ('phase', 0.1)]:
            with self.subTest(key=key):
                rows=phase_capture(); rows[5][key]=value
                self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'fail')

    def test_rejects_slow_frames_and_short_runs(self):
        rows=capture()
        self.assertEqual(evaluate(rows)['status'], 'fail')
        for row in rows: row['present_seconds'] *= 2
        self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'fail')

    def test_invalid_time(self):
        for value in (float('nan'), 0, -1, float('inf')):
            rows=capture(); rows[5]['present_seconds']=value
            with self.subTest(value=value), self.assertRaises(ValueError): evaluate(rows)


if __name__ == '__main__': unittest.main()
