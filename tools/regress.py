"""Run the regression routes on a build, score them and compare with a base build.

    python tools/regress.py build/v121 --repeat 3 --out build/regress/v121
    python tools/regress.py build/v121 --routes island --cold
    python tools/regress.py build/v121 --routes field --validation

Each run writes <out>/<build name>/<route>-<i>/ (log.txt, shots/, score.json); the summary prints median and worst per
metric and route for every build, and with --base a diff table (ok, better, worse). Runs are nondeterministic: a
verdict compares medians. A candidate build without a pipeline cache, pipeline-keys.bin or shader-cache gets the
base build's copy before its first warm run. Every run works on a private copy of the pipeline cache, so the builds'
own files stay as they are (TOTK_SHADER_CACHE still points at the build's directory, which the game extends).
--cold starts the runs with an empty pipeline cache and key file; translated shaders stay.
"""
import argparse
import datetime
import json
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

sys.path.insert(0, str(Path(__file__).resolve().parent))
from regress_score import far_gradient, is_scored, parse_perf, score_frames, vuid_counts  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SAVES = ROOT / 'build' / 'save-backup'
SLOW_MS = 50
SHOT_STEP = 5
TAIL_FRAMES = 30        # shots examined for a dead or stuck ending
TAIL_BLACK = 0.9
BLACK_LUMA = 12
END_SLACK = 120         # frames the log may stop short of the requested count

# name: (save, script, frames, scored window)
ROUTES = {
    'fight': ('user-save-2026-10-21-monsters', 'tools/input/fight.txt', 2600, (1560, 2600)),
    'rock': ('user-save-2026-10-21-monsters', 'build/recordings/rock-2026-10-22.txt', 3300, (1500, 3300)),
    'island': ('user-save-2026-10-22-island', 'tools/input/island.txt', 3600, (1500, 3600)),
    'field': ('user-save-2026-10-21-field', 'tools/input/load_save.txt', 2400, (1560, 2400)),
}

# metric: (label, lower is better, absolute tolerance, relative tolerance)
METRICS = {
    'fps_mean': ('fps', False, 1.0, 0.03),
    'max_frame_ms': ('max frame ms', True, 5.0, 0.15),
    'slow_count': (f'frames >{SLOW_MS} ms', True, 1, 0.15),
    'render_busy_ms': ('render busy ms', True, 0.5, 0.10),
    'replay_ms': ('replay ms', True, 0.5, 0.10),
    'black_frames': ('black-block frames', True, 0, 0),
    'flat_frames': ('flat frames', True, 0, 0),
    'vuid_total': ('validation messages', True, 0, 0),
}
CACHE_FILES = ('pipeline-cache.bin', 'pipeline-keys.bin')


def seed_caches(build, base):
    """Copy the base build's caches into a candidate that lacks them; returns what was copied."""
    seeded = []
    if build.resolve() == base.resolve():
        return seeded
    for name in (*CACHE_FILES, 'shader-cache'):
        src, dst = base / name, build / name
        if dst.exists() or not src.exists():
            continue
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
        seeded.append(name)
    return seeded


def run_route(build, route, i, out, cold, validation):
    save, script, frames, (lo, hi) = ROUTES[route]
    run = out / build.name / f'{route}-{i}'
    shutil.rmtree(run, ignore_errors=True)
    shots = run / 'shots'
    cache = run / 'cache'
    shots.mkdir(parents=True)
    cache.mkdir()
    if not cold:
        for name in CACHE_FILES:
            if (build / name).exists():
                shutil.copy2(build / name, cache / name)
    env = {
        'TOTK_TRACE': 'perf,sched',
        'TOTK_SHOTS': f'{lo}-{hi}:{SHOT_STEP}',
        'TOTK_SHOTS_DIR': str(shots),
        'TOTK_PIPELINE_CACHE': str(cache / 'pipeline-cache.bin'),
    }
    if (build / 'shader-cache').is_dir():
        env['TOTK_SHADER_CACHE'] = str(build / 'shader-cache')
    if validation:
        env['TOTK_VK_VALIDATION'] = '1'
    log = run / 'log.txt'
    cmd = [sys.executable, str(ROOT / 'tools' / 'run_game.py'), str(build), '--save', save,
           '--script', str(ROOT / script), '--frames', str(frames), '--quiet', '--tag', f'rg-{route}-{i}',
           '--log', str(log)]
    for k, v in env.items():
        cmd += ['--env', f'{k}={v}']
    rc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    score = score_run(log, shots, lo, hi, frames, rc)
    score['run'] = str(run)
    (run / 'score.json').write_text(json.dumps(score, indent=1))
    return score


def black_fraction(img):
    grey = img.convert('L')
    return sum(grey.histogram()[:BLACK_LUMA]) / (grey.width * grey.height)


def score_run(log, shots, lo, hi, frames, rc):
    perf = parse_perf(log, lo, hi)
    text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    paths = {int(p.stem[1:]): p for p in shots.glob('f*.png')}
    fr = score_frames(paths) if paths else {'black_counts': {}, 'flat_frames': [], 'scored': 0, 'total': 0}
    vuids = vuid_counts(text)
    last = [n for n in sorted(paths)][-TAIL_FRAMES:]
    tail_black = 0.0
    if last:
        fractions = []
        for n in last:
            with Image.open(paths[n]) as im:
                fractions.append(black_fraction(im.convert('RGB')))
        tail_black = statistics.mean(fractions)
    last_frame = 0
    for line in text.splitlines():
        if 'perf: frame ' in line:
            last_frame = int(line.split('perf: frame ')[1].split(':')[0])
    reasons = []
    if tail_black > TAIL_BLACK:
        reasons.append(f'last {len(last)} shots {tail_black:.0%} black (game over?)')
    if perf['crashed']:
        reasons.append('crashed or no perf lines')
    if rc != 0:
        reasons.append(f'exit code {rc}')
    if last_frame < frames - END_SLACK:
        reasons.append(f'log stops at frame {last_frame} of {frames}')
    return {
        'rc': rc, 'fps_mean': perf['fps_mean'], 'max_frame_ms': perf['max_frame_ms'],
        'slow_count': sum(1 for _, ms, _ in perf['slow_frames'] if ms >= SLOW_MS),
        'slow_frames': [(n, ms, cls) for n, ms, cls in perf['slow_frames'] if ms >= SLOW_MS],
        'render_busy_ms': perf['render_busy_ms'], 'replay_ms': perf['replay_ms'],
        'black_frames': len(fr['black_counts']), 'black_counts': fr['black_counts'],
        'flat_frames': len(fr['flat_frames']), 'flat_list': fr['flat_frames'],
        'scored': fr['scored'], 'shots': fr['total'],
        'vuids': vuids, 'vuid_total': sum(vuids.values()),
        'route_failed': bool(reasons), 'fail_reasons': reasons,
    }


def contact_sheet(scores, path):
    """Up to 6 worst frames over the runs of one route: black blocks first, then flat, then the lowest far gradient."""
    cands = []
    for s in scores:
        shots = Path(s['run']) / 'shots'
        for n, c in s['black_counts'].items():
            cands.append((0, -c, s['run'], int(n), f'black x{c}'))
        for n in s['flat_list']:
            cands.append((1, 0, s['run'], int(n), 'flat'))
    cands.sort()
    chosen = cands[:6]
    if len(chosen) < 6 and scores:
        have = {(r, n) for _, _, r, n, _ in chosen}
        rest = []
        for p in sorted((Path(scores[0]['run']) / 'shots').glob('f*.png')):
            with Image.open(p) as im:
                im = im.convert('RGB')
                if is_scored(im):
                    rest.append((far_gradient(im), scores[0]['run'], int(p.stem[1:])))
        rest.sort()
        for g, r, n in rest:
            if len(chosen) == 6:
                break
            if (r, n) not in have:
                chosen.append((2, g, r, n, f'lowest far gradient {g:.1f}'))
    if not chosen:
        return False
    sheet = Image.new('RGB', (3 * 480, 2 * 270))
    for k, (_, _, run, n, label) in enumerate(chosen):
        with Image.open(Path(run) / 'shots' / f'f{n:05d}.png') as im:
            tile = im.convert('RGB')
        d = ImageDraw.Draw(tile)
        d.rectangle((0, 0, 330, 14), fill=(0, 0, 0))
        d.text((2, 2), f'{Path(run).name} f{n} {label}', fill=(255, 255, 0))
        sheet.paste(tile, ((k % 3) * 480, (k // 3) * 270))
    sheet.save(path)
    return True


def stat(values, lower_is_better):
    """(median, worst) over the runs that report the metric; None when none does."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None, None
    return statistics.median(vals), (max(vals) if lower_is_better else min(vals))


def fmt(v):
    return 'n/a' if v is None else (f'{v:.1f}' if isinstance(v, float) else str(v))


def summarize(name, results):
    """results: {route: [score]} -> {route: {metric: (median, worst)}}; prints the table."""
    table = {r: {m: stat([s[m] for s in scs], METRICS[m][1]) for m in METRICS} for r, scs in results.items()}
    print(f'\n== {name}: median / worst of {max(len(s) for s in results.values())} runs')
    print(f'{"route":8}' + ''.join(f'{METRICS[m][0]:>24}' for m in METRICS) + '  failed')
    for r, row in table.items():
        failed = sum(s['route_failed'] for s in results[r])
        print(f'{r:8}' + ''.join(f'{fmt(row[m][0]) + " / " + fmt(row[m][1]):>24}' for m in METRICS) + f'  {failed}')
    for r, scs in results.items():
        for s in scs:
            if s['route_failed']:
                print(f'  ROUTE FAILED {s["run"]}: {"; ".join(s["fail_reasons"])}')
    return table


def verdict(m, cand, base):
    if cand is None or base is None:
        return 'n/a'
    _, lower, tol_abs, tol_rel = METRICS[m]
    delta = cand - base if lower else base - cand
    slack = max(tol_abs, tol_rel * abs(base))
    if delta > slack:
        return 'worse'
    if delta < -slack:
        return 'better'
    return 'ok'


def diff_table(cand, base, cand_results, base_results):
    print('\n== diff against base (median; verdict)')
    print(f'{"route":8}' + ''.join(f'{METRICS[m][0]:>24}' for m in METRICS))
    for r in cand:
        cells = []
        for m in METRICS:
            c, b = cand[r][m][0], base.get(r, {}).get(m, (None,))[0]
            cells.append(f'{fmt(c)} vs {fmt(b)} {verdict(m, c, b)}')
        print(f'{r:8}' + ''.join(f'{c:>24}' for c in cells))
        new = set().union(*(s['vuids'] for s in cand_results[r])) - set().union(*(s['vuids'] for s in base_results.get(r, [])))
        if new:
            print(f'  new validation ids in {r}: {", ".join(sorted(new))}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build', type=Path)
    ap.add_argument('--base', type=Path, help='frozen build to compare with (e.g. build/v121-base)')
    ap.add_argument('--routes', default='fight,rock,island,field')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--cold', action='store_true', help='empty pipeline cache and key file')
    ap.add_argument('--validation', action='store_true', help='TOTK_VK_VALIDATION=1; counts VUIDs')
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    out = (a.out or ROOT / 'build' / 'regress' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')).resolve()
    routes = a.routes.split(',')
    for r in routes:
        if r not in ROUTES:
            sys.exit(f'unknown route {r}; choose from {", ".join(ROUTES)}')
        if not (SAVES / ROUTES[r][0]).is_dir() or not (ROOT / ROUTES[r][1]).exists():
            sys.exit(f'route {r}: missing save or script')
    builds = [a.build.resolve()] + ([a.base.resolve()] if a.base else [])
    if a.base and not a.cold:
        seeded = seed_caches(builds[0], builds[1])
        print('caches seeded from base:', ', '.join(seeded) if seeded else 'none (the build had its own)')
    tables, all_results = {}, {}
    for build in builds:
        results = {}
        for r in routes:
            results[r] = [run_route(build, r, i, out, a.cold, a.validation) for i in range(a.repeat)]
            sheet = out / build.name / f'{r}-worst.png'
            contact_sheet(results[r], sheet)
        all_results[build.name] = results
        tables[build.name] = summarize(build.name, results)
    if a.base:
        diff_table(tables[builds[0].name], tables[builds[1].name], all_results[builds[0].name], all_results[builds[1].name])
    (out / 'summary.json').write_text(json.dumps(tables, indent=1))
    print(f'\nwrote {out}')
    bad = any(s['route_failed'] for res in all_results.values() for scs in res.values() for s in scs)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
