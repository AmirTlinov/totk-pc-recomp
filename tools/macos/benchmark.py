"""Check presentation cadence and immutable guest/display clock evidence.

State IDs establish ordering, not image equivalence. A phase label alone cannot
certify a new rendered state; phase acceptance needs a reference-render check.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


# periodic_clock.c bounds one completed guest step to this duration. A capture
# may start/end with one pending step and a bounded amount of catch-up debt;
# longer unobserved debt cannot be assumed to excuse a wrong simulation rate.
MAX_SIMULATION_STEP_NS = 1_000_000_000 // 15


def simulation_progress(indices, simulation, steps, requested, times, problems):
    result = dict(simulation_time_rate=None, simulation_requested_time_rate=None,
                  simulation_advancement_error_ms=None, simulation_advancement_peak_error_ms=None,
                  simulation_requested_advancement_error_ms=None,
                  simulation_advancement_tolerance_ms=None,
                  simulation_debt_allowance_ms=MAX_SIMULATION_STEP_NS / 1e6)
    if len(indices) < 2:
        problems.append('at least two full anchors are required to verify simulation advancement')
        return result
    first, last = indices[0], indices[-1]
    actual = [round(time * 1e9) for time in times]
    precision_ns = 2 * math.ceil(1 + max(math.ulp(time) for time in times) * 1e9)
    peak_error = 0
    for i in indices[1:]:
        # This fixed boundary allowance never grows with capture length. Check
        # every prefix so a slow interval followed by fast catch-up cannot hide
        # an arbitrarily large drift in an apparently correct final average.
        allowance = (MAX_SIMULATION_STEP_NS + min(steps[first], MAX_SIMULATION_STEP_NS)
                     + min(steps[i], MAX_SIMULATION_STEP_NS) + precision_ns)
        advanced = simulation[i] - simulation[first]
        actual_elapsed = actual[i] - actual[first]
        if actual_elapsed <= 0:
            raise ValueError('anchor presentation timestamps do not advance at nanosecond precision')
        error = advanced - actual_elapsed
        peak_error = max(peak_error, abs(error))
        if abs(error) > allowance:
            problems.append('simulation advancement differs from real presentation time')
        if requested[first] and requested[i] and requested[i] > requested[first]:
            requested_elapsed = requested[i] - requested[first]
            requested_error = advanced - requested_elapsed
            if abs(requested_error) > allowance:
                problems.append('simulation advancement differs from requested presentation time')
            if i == last:
                result['simulation_requested_time_rate'] = advanced / requested_elapsed
                result['simulation_requested_advancement_error_ms'] = requested_error / 1e6
        if i == last:
            result['simulation_time_rate'] = advanced / actual_elapsed
            result['simulation_advancement_error_ms'] = error / 1e6
            result['simulation_advancement_tolerance_ms'] = allowance / 1e6
    result['simulation_advancement_peak_error_ms'] = peak_error / 1e6
    return result


def clock_evidence(rows, times, kinds, anchors, problems):
    names = ('simulation_generation', 'simulation_time_ns', 'completed_step_ns',
             'display_phase_time_ns', 'requested_presentation_time_ns')
    values = {}
    for name in names:
        if any(name not in row for row in rows):
            raise ValueError(f'missing trace clock field: {name}')
        try:
            values[name] = [int(row[name]) for row in rows]
        except (ValueError, TypeError) as error:
            raise ValueError(f'invalid trace clock field: {name}') from error
        if any(value < 0 or value > 2**64 - 1 for value in values[name]):
            raise ValueError(f'invalid trace clock field: {name}')
    generations, simulation, steps, phase_times, requested = (values[name] for name in names)
    if any(not generation or not time for generation, time in zip(generations, simulation)):
        problems.append('missing completed simulation stamp')
    if any(not time for time in phase_times):
        problems.append('missing display phase time')
    if any(not time for time in requested):
        problems.append('requested presentation time is unknown')
    known_targets = [time for time in requested if time]
    if any(b <= a for a, b in zip(known_targets, known_targets[1:])):
        problems.append('repeated or reordered requested presentation times')
    if any(b < a for a, b in zip(generations, generations[1:])):
        problems.append('reordered simulation generations')
    if any(b < a for a, b in zip(simulation, simulation[1:])):
        problems.append('reordered simulation times')
    previous_anchor = None
    dropped_anchors = 0
    for i, kind in enumerate(kinds):
        if kind == 'anchor':
            if not steps[i] or steps[i] > simulation[i] or steps[i] > MAX_SIMULATION_STEP_NS:
                problems.append('invalid completed simulation step')
            if phase_times[i] != simulation[i]:
                problems.append('anchor phase time differs from completed simulation time')
            if previous_anchor is not None:
                j = previous_anchor
                if generations[i] != generations[j] + 1:
                    problems.append('missing or repeated simulation generations between anchors')
                elif simulation[i] - simulation[j] != steps[i]:
                    problems.append('completed step differs from simulation time advance')
                dropped_anchors += max(0, generations[i] - generations[j] - 1)
                if anchors[i] != anchors[j] + 1:
                    problems.append('missing or repeated anchor frame identities')
            previous_anchor = i
        elif kind in ('camera', 'phase'):
            if steps[i]:
                problems.append('display phase claims a completed guest step')
            if phase_times[i] <= simulation[i]:
                problems.append('display phase does not follow its completed simulation state')
            if previous_anchor is not None:
                j = previous_anchor
                if (anchors[i], generations[i], simulation[i]) != (anchors[j], generations[j], simulation[j]):
                    problems.append('display phase differs from its source anchor stamp')
                if requested[i] and requested[j] and phase_times[i] - simulation[i] != requested[i] - requested[j]:
                    problems.append('display phase time differs from requested presentation advance')
    elapsed = times[-1] - times[0]
    error_ms = [(time - target * 1e-9) * 1000 for time, target in zip(times, requested) if target]
    progress = simulation_progress([i for i, kind in enumerate(kinds) if kind == 'anchor'],
                                   simulation, steps, requested, times, problems)
    return dict(simulation_hz=(generations[-1] - generations[0]) / elapsed,
                simulation_cpu_ms=None,
                dropped_anchor_count=dropped_anchors,
                unknown_requested_presentation_times=sum(not time for time in requested),
                presentation_target_error_p50_ms=statistics.median(error_ms) if error_ms else None,
                presentation_target_error_max_ms=max(error_ms) if error_ms else None, **progress)


def evaluate(rows, target=144.0, minimum_seconds=60.0):
    if not math.isfinite(target) or target <= 0 or not math.isfinite(minimum_seconds) or minimum_seconds <= 0:
        raise ValueError('positive, finite target and duration required')
    if len(rows) < 2:
        raise ValueError('at least two presented frames are required')
    problems = []
    times = [float(row['present_seconds']) for row in rows]
    if any(not math.isfinite(t) or t <= 0 for t in times):
        raise ValueError('invalid presentation timestamp')
    # Drawable callbacks may arrive on different threads. Their timestamps,
    # rather than the order in which callbacks acquired the log lock, define
    # physical presentation order.
    rows = [row for _, row in sorted(zip(times, rows), key=lambda pair: pair[0])]
    times.sort()
    tickets = [int(row['ticket']) for row in rows]
    frames = [int(row['frame']) for row in rows]
    kinds = [row['kind'] for row in rows]
    anchors = [int(row['anchor_id']) for row in rows]
    phases = [float(row['phase']) for row in rows]
    generations = [int(row['state_generation']) for row in rows]
    if any(b != a + 1 for a, b in zip(tickets, tickets[1:])):
        problems.append('missing, repeated or reordered present tickets')
    if len(set(generations)) != len(generations) or any(b <= a for a, b in zip(generations, generations[1:])):
        problems.append('repeated or reordered rendered states')
    if any(g <= 0 for g in generations):
        problems.append('invalid rendered state generation')
    if any(frame != anchor for frame, anchor in zip(frames, anchors)):
        problems.append('frame identity differs from anchor identity')
    if any(b < a for a, b in zip(anchors, anchors[1:])):
        problems.append('reordered anchor states')
    for index, (kind, phase) in enumerate(zip(kinds, phases)):
        if not math.isfinite(phase):
            raise ValueError('invalid display phase')
        if kind == 'anchor':
            if phase != 1.0: problems.append('anchor state does not end its display phase')
        elif kind == 'phase':
            if not 0.0 < phase < 1.0: problems.append('phase state is outside its anchor interval')
            problems.append('phase states lack reference-render validation evidence')
        elif kind == 'camera':
            problems.append('camera-only reprojection is not a complete rendered state')
        else:
            problems.append('unknown rendered state kind')
        if index and anchors[index] == anchors[index - 1] and kinds[index - 1] != 'anchor' and phase <= phases[index - 1]:
            problems.append('repeated or reordered display phases')
    if any(int(row['strict']) != 1 for row in rows):
        problems.append('non-strict rendering mode')
    for name in ('scene', 'output', 'drawable'):
        if any((int(r[name + '_width']), int(r[name + '_height'])) != (3840, 2160) for r in rows):
            problems.append(f'{name} resolution is unchecked or differs from 3840x2160')
    intervals = [b - a for a, b in zip(times, times[1:])]
    if any(dt <= 0 for dt in intervals):
        raise ValueError('presentation timestamps must strictly increase')
    elapsed = times[-1] - times[0]
    if elapsed < minimum_seconds:
        problems.append(f'capture is shorter than {minimum_seconds:g} seconds')
    ordered = sorted(intervals)
    p99 = ordered[math.ceil(0.99 * len(ordered)) - 1]
    worst_one_percent = ordered[-max(1, math.ceil(len(ordered) * 0.01)):]
    fps = len(intervals) / elapsed
    # The current native renderer has only full anchors and camera reprojection.
    # No phase producer yet supplies reference validation, so unverified labels
    # cannot inflate this rate even when every state ID differs.
    rendered_count = sum(kind == 'anchor' for kind in kinds[1:])
    budget = 1 / target
    # CSV timestamps have nine decimal places. Include one decimal quantum and
    # one floating point ULP; this is nanoseconds, not a frame-rate allowance.
    precision = 1e-9 + max(math.ulp(t) for t in times)
    if elapsed > len(intervals) / target + precision: problems.append('average presented FPS is below target')
    if elapsed > rendered_count / target + precision: problems.append('rendered state rate is below target')
    if p99 > budget + precision: problems.append('p99 frame time exceeds the frame budget')
    clocks = clock_evidence(rows, times, kinds, anchors, problems)
    return dict(status='pass' if not problems else 'fail', reasons=list(dict.fromkeys(problems)), frames=len(rows),
                seconds=elapsed, average_fps=fps, rendered_state_hz=rendered_count / elapsed,
                anchor_hz=sum(kind == 'anchor' for kind in kinds[1:]) / elapsed,
                unverified_phase_hz=sum(kind == 'phase' for kind in kinds[1:]) / elapsed,
                presented_hz=fps, p50_ms=statistics.median(intervals) * 1000,
                p99_ms=p99 * 1000, low_1_percent_fps=1 / statistics.mean(worst_one_percent),
                worst_ms=max(intervals) * 1000, over_budget=sum(dt > budget + precision for dt in intervals),
                target_fps=target, budget_ms=budget * 1000, timestamp_precision_ns=precision * 1e9,
                evidence_scope='presentation cadence and clock consistency; IDs do not prove image correctness',
                reference_render_verified=False, **clocks)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    ap.add_argument('--target', type=float, default=144)
    ap.add_argument('--minimum-seconds', type=float, default=60)
    ap.add_argument('--start-ticket', type=int, help='explicit start of the measured gameplay segment')
    ap.add_argument('--end-ticket', type=int)
    ap.add_argument('--output', type=Path)
    a = ap.parse_args()
    with a.trace.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    selected = [r for r in rows if (a.start_ticket is None or int(r['ticket']) >= a.start_ticket)
                and (a.end_ticket is None or int(r['ticket']) <= a.end_ticket)]
    result = evaluate(selected, a.target, a.minimum_seconds)
    result.update(trace=str(a.trace), start_ticket=a.start_ticket, end_ticket=a.end_ticket,
                  excluded_frames=len(rows) - len(selected))
    text = json.dumps(result, indent=2) + '\n'
    if a.output: a.output.write_text(text)
    print(text, end='')
    return 0 if result['status'] == 'pass' else 1


if __name__ == '__main__': raise SystemExit(main())
