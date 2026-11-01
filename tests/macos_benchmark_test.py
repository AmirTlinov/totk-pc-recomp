import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from macos.benchmark import evaluate


def capture():
    # 160 Hz synthetic timestamps, enough headroom to avoid floating point ties.
    return [dict(ticket=i+1, frame=i+1, kind='anchor', anchor_id=i+1, phase=1.0,
                 state_generation=i+1, present_seconds=1+i/160, strict=1,
                 simulation_generation=i+1, simulation_time_ns=(i+1)*6250000,
                 completed_step_ns=6250000, display_phase_time_ns=(i+1)*6250000,
                 requested_presentation_time_ns=1000000000+i*6250000,
                 scene_width=3840, scene_height=2160, output_width=3840, output_height=2160,
                 drawable_width=3840, drawable_height=2160) for i in range(321)]


def phase_capture():
    phases = (1.0, 1 / 3, 2 / 3)
    return [dict(ticket=i+1, frame=i//3+1, kind='anchor' if i%3 == 0 else 'phase',
                 anchor_id=i//3+1, phase=phases[i%3], state_generation=i+1,
                 simulation_generation=i//3+1, simulation_time_ns=(i//3+1)*18750000,
                 completed_step_ns=18750000 if i%3 == 0 else 0,
                 display_phase_time_ns=(i//3+1)*18750000+(i%3)*6250000,
                 requested_presentation_time_ns=1000000000+i*6250000,
                 present_seconds=1+i/160, strict=1, scene_width=3840, scene_height=2160,
                 output_width=3840, output_height=2160, drawable_width=3840, drawable_height=2160)
            for i in range(321)]


class PresentationTests(unittest.TestCase):
    def test_native_rendered_frames(self):
        self.assertEqual(evaluate(capture(), minimum_seconds=1)['status'], 'pass')

    def test_phase_label_does_not_prove_a_correct_rendered_state(self):
        result = evaluate(phase_capture(), minimum_seconds=1)
        self.assertEqual(result['status'], 'fail')
        self.assertIn('phase states lack reference-render validation evidence', result['reasons'])
        self.assertEqual(result['rendered_state_hz'], result['anchor_hz'])
        self.assertFalse(result['reference_render_verified'])

    def test_camera_reprojection_cannot_count_as_temporal_states(self):
        rows = phase_capture()
        for row in rows:
            if row['kind'] == 'phase': row['kind'] = 'camera'
        result = evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'fail')
        self.assertIn('camera-only reprojection is not a complete rendered state', result['reasons'])
        self.assertAlmostEqual(result['rendered_state_hz'], result['anchor_hz'])
        self.assertLess(result['rendered_state_hz'], result['presented_hz'] / 2)

    def test_exact_144_hz_survives_csv_timestamp_rounding(self):
        rows=capture()
        for i, row in enumerate(rows):
            row['present_seconds']=f'{20000+i/144:.9f}'
            row['requested_presentation_time_ns']=20_000_000_000_000+i*1_000_000_000//144
            row['simulation_time_ns']=(i+1)*1_000_000_000//144
            row['display_phase_time_ns']=row['simulation_time_ns']
            row['completed_step_ns']=row['simulation_time_ns']-i*1_000_000_000//144
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'pass', result)
        self.assertEqual(result['over_budget'], 0)
        self.assertLess(result['timestamp_precision_ns'], 2)
        self.assertAlmostEqual(result['simulation_time_rate'], 1, places=8)
        self.assertAlmostEqual(result['simulation_requested_time_rate'], 1, places=8)

    def test_slow_and_fast_simulation_fail_with_consistent_ids_and_steps(self):
        for numerator, denominator in ((1,100), (9,10), (11,10), (2,1)):
            with self.subTest(rate=numerator/denominator):
                rows=capture()
                for row in rows:
                    for key in ('simulation_time_ns', 'completed_step_ns', 'display_phase_time_ns'):
                        row[key]=row[key]*numerator//denominator
                result=evaluate(rows, minimum_seconds=1)
                self.assertEqual(result['status'], 'fail')
                self.assertIn('simulation advancement differs from real presentation time', result['reasons'])
                self.assertAlmostEqual(result['simulation_time_rate'], numerator/denominator)
                self.assertIsNone(result['simulation_cpu_ms'])

    def test_simulation_rate_is_tied_to_actual_presentation_even_if_targets_match_wrong_rate(self):
        rows=capture()
        for row in rows:
            for key in ('simulation_time_ns', 'completed_step_ns', 'display_phase_time_ns'):
                row[key] //= 100
            row['requested_presentation_time_ns'] = 1_000_000_000 + (row['requested_presentation_time_ns']-1_000_000_000)//100
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['simulation_requested_time_rate'], 1)
        self.assertIn('simulation advancement differs from real presentation time', result['reasons'])

    def test_final_average_cannot_hide_excessive_mid_capture_simulation_drift(self):
        rows=capture(); time=0
        for i, row in enumerate(rows):
            step=6250000 if i==0 else 3125000 if i<=160 else 9375000
            time+=step
            row.update(simulation_time_ns=time, display_phase_time_ns=time, completed_step_ns=step)
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['simulation_time_rate'], 1)
        self.assertGreater(result['simulation_advancement_peak_error_ms'], 400)
        self.assertIn('simulation advancement differs from real presentation time', result['reasons'])

    def test_endpoint_allowance_is_bounded_and_oversized_steps_do_not_expand_it(self):
        rows=capture()
        for row in rows:
            row['simulation_time_ns']+=10_000_000_000
            row['display_phase_time_ns']=row['simulation_time_ns']
        rows[0]['completed_step_ns']=10_000_000_000
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'fail')
        self.assertIn('invalid completed simulation step', result['reasons'])
        self.assertLess(result['simulation_advancement_tolerance_ms'], 140)

    def test_small_endpoint_debt_is_reported_with_fixed_time_tolerance(self):
        rows=capture()
        rows[-1]['completed_step_ns']+=10_000_000
        rows[-1]['simulation_time_ns']+=10_000_000
        rows[-1]['display_phase_time_ns']+=10_000_000
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'pass', result)
        self.assertEqual(result['simulation_advancement_error_ms'], 10)
        self.assertLess(result['simulation_advancement_tolerance_ms'], 100)

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
        rows=capture()[:2]
        rows[1]['present_seconds']=rows[0]['present_seconds']+1e-11
        with self.assertRaisesRegex(ValueError, 'nanosecond precision'): evaluate(rows)

    def test_three_clock_domains_are_reported_separately(self):
        rows=phase_capture()
        for row in rows:
            if row['kind'] == 'phase': row['kind']='camera'
        result=evaluate(rows, minimum_seconds=1)
        self.assertAlmostEqual(result['simulation_hz'], result['anchor_hz'])
        self.assertGreater(result['presented_hz'], result['simulation_hz'] * 2.9)
        self.assertIsNone(result['simulation_cpu_ms'])
        self.assertAlmostEqual(result['presentation_target_error_p50_ms'], 0)
        self.assertFalse(any('stamp' in reason or 'advance' in reason for reason in result['reasons']))

    def test_display_ticks_do_not_advance_completed_simulation(self):
        rows=phase_capture()
        rows[1]['simulation_generation'] += 1
        result=evaluate(rows, minimum_seconds=1)
        self.assertIn('display phase differs from its source anchor stamp', result['reasons'])
        rows=phase_capture(); rows[1]['completed_step_ns']=6250000
        result=evaluate(rows, minimum_seconds=1)
        self.assertIn('display phase claims a completed guest step', result['reasons'])

    def test_rejects_inconsistent_clock_evidence(self):
        for key, value in [('simulation_generation', 0), ('simulation_time_ns', 0),
                           ('completed_step_ns', 1), ('display_phase_time_ns', 1),
                           ('requested_presentation_time_ns', 1)]:
            with self.subTest(key=key):
                rows=capture(); rows[5][key]=value
                self.assertEqual(evaluate(rows, minimum_seconds=1)['status'], 'fail')
        rows=phase_capture(); rows[1]['display_phase_time_ns'] += 1
        self.assertIn('display phase time differs from requested presentation advance',
                      evaluate(rows, minimum_seconds=1)['reasons'])

    def test_unknown_fifo_deadline_is_explicit(self):
        rows=capture()
        for row in rows: row['requested_presentation_time_ns']=0
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['status'], 'fail')
        self.assertEqual(result['unknown_requested_presentation_times'], len(rows))
        self.assertIsNone(result['presentation_target_error_p50_ms'])
        self.assertIn('requested presentation time is unknown', result['reasons'])

    def test_coalesced_guest_anchor_is_visible_despite_contiguous_present_tickets(self):
        rows=capture()
        del rows[5]
        for i, row in enumerate(rows): row['ticket']=i+1
        result=evaluate(rows, minimum_seconds=1)
        self.assertEqual(result['dropped_anchor_count'], 1)
        self.assertIn('missing or repeated simulation generations between anchors', result['reasons'])
        self.assertNotIn('missing, repeated or reordered present tickets', result['reasons'])

    def test_old_or_invalid_clock_schema_is_rejected(self):
        for key in ('simulation_generation', 'simulation_time_ns', 'completed_step_ns',
                    'display_phase_time_ns', 'requested_presentation_time_ns'):
            with self.subTest(key=key):
                rows=capture(); del rows[5][key]
                with self.assertRaisesRegex(ValueError, 'missing trace clock field'): evaluate(rows)
                for value in (-1, 2**64, 'nan'):
                    rows=capture(); rows[5][key]=value
                    with self.assertRaisesRegex(ValueError, 'invalid trace clock field'): evaluate(rows)


if __name__ == '__main__': unittest.main()
