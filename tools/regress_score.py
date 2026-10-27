"""Score a game log and its TOTK_SHOTS frames for tools/regress.py.

Log metrics come from the TOTK_TRACE=perf lines (runtime/prof_win.c `perf_present`); frame metrics look for the two
glitches it targets: near-black blocks inside a lit picture and a far field that lost its detail.
"""
import re
import statistics
from collections import Counter

from PIL import Image, ImageChops, ImageStat

SLOW = re.compile(r'perf: slow frame (\d+): ([\d.]+) ms; blocked pipeline ([\d.]+) xlate ([\d.]+) texture ([\d.]+) '
                  r'astc ([\d.]+) fence ([\d.]+) pacing ([\d.]+)')
FRAME = re.compile(r'perf: frame (\d+): interval avg ([\d.]+) ms max ([\d.]+) ms \(([\d.]+) fps\)')
BUSY = re.compile(r'render busy avg ([\d.]+) ms')
REPLAY = re.compile(r'replay avg ([\d.]+) ms')
FATAL = re.compile(r'FATAL|rt_fatal')
VUID = re.compile(r'VUID-[A-Za-z0-9_-]+')

BLOCK = 16
BLACK_MAX = 12        # a block is black when no channel reaches this
NEIGHBOUR_LUMA = 60   # and lit neighbours are brighter than this
LUMA_LO, LUMA_HI = 25, 240   # frames outside are loading screens, menus, game over or white-outs: not scored
FLAT_RATIO = 0.4
FLAT_NEIGHBOURS = 10
BLOCKED_MIN_MS = 5.0  # a slow frame with less blocked time in every class is "other"


def parse_perf_text(text, lo, hi):
    """Metrics of the frames in [lo, hi] from a log's text."""
    fps, maxes, busy, replay, slow = [], [], [], [], []
    seen_after_lo = False
    crashed = False
    for line in text.splitlines():
        if FATAL.search(line):
            crashed = True
        if m := SLOW.search(line):
            n = int(m[1])
            if lo <= n <= hi:
                ms = float(m[2])
                classes = {'pipeline': float(m[3]), 'xlate': float(m[4]), 'texture': float(m[5]), 'fence': float(m[7])}
                name, blocked = max(classes.items(), key=lambda kv: kv[1])
                slow.append((n, ms, name if blocked >= BLOCKED_MIN_MS else 'other'))
        elif m := FRAME.search(line):
            n = int(m[1])
            if n > lo:
                seen_after_lo = True
            if lo < n <= hi:
                fps.append(float(m[4]))
                maxes.append(float(m[3]))
                if b := BUSY.search(line):
                    busy.append(float(b[1]))
                if r := REPLAY.search(line):
                    replay.append(float(r[1]))
    max_ms = max([ms for _, ms, _ in slow] + maxes, default=0.0)
    return {
        'fps_mean': statistics.mean(fps) if fps else 0.0,
        'max_frame_ms': max_ms,
        'slow_frames': slow,
        'render_busy_ms': statistics.mean(busy) if busy else None,
        'replay_ms': statistics.mean(replay) if replay else None,
        'crashed': crashed or not seen_after_lo,
    }


def parse_perf(path, lo, hi):
    with open(path, encoding='utf-8', errors='replace') as f:
        return parse_perf_text(f.read(), lo, hi)


def vuid_counts(log):
    return dict(Counter(VUID.findall(log)))


def luma_mean(img):
    return ImageStat.Stat(img.convert('L')).mean[0]


def is_scored(img):
    return LUMA_LO <= luma_mean(img) <= LUMA_HI


def black_blocks(img):
    """16x16 blocks that are black while at least 3 of their 4 neighbours are lit."""
    rgb = img.convert('RGB')
    grey = rgb.convert('L')
    cols, rows = rgb.width // BLOCK, rgb.height // BLOCK
    black = [[False] * cols for _ in range(rows)]
    luma = [[0.0] * cols for _ in range(rows)]
    for r in range(rows):
        for c in range(cols):
            box = (c * BLOCK, r * BLOCK, (c + 1) * BLOCK, (r + 1) * BLOCK)
            black[r][c] = max(hi for _, hi in rgb.crop(box).getextrema()) < BLACK_MAX
            luma[r][c] = ImageStat.Stat(grey.crop(box)).mean[0]
    count = 0
    for r in range(rows):
        for c in range(cols):
            if not black[r][c]:
                continue
            lit = sum(1 for rr, cc in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1))
                      if 0 <= rr < rows and 0 <= cc < cols and luma[rr][cc] > NEIGHBOUR_LUMA)
            count += lit >= 3
    return count


def far_gradient(img):
    """Mean absolute horizontal plus vertical luma difference over the top half (sky and far terrain)."""
    top = img.convert('L').crop((0, 0, img.width, img.height // 2))
    w, h = top.size
    dx = ImageChops.difference(top.crop((0, 0, w - 1, h)), top.crop((1, 0, w, h)))
    dy = ImageChops.difference(top.crop((0, 0, w, h - 1)), top.crop((0, 1, w, h)))
    return ImageStat.Stat(dx).mean[0] + ImageStat.Stat(dy).mean[0]


def flat_frames(frames):
    """Frame numbers whose far_gradient is under FLAT_RATIO x the median of the nearest scored frames."""
    scored = [(n, far_gradient(img)) for n, img in frames if is_scored(img)]
    flat = []
    for i, (n, g) in enumerate(scored):
        near = sorted((j for j in range(len(scored)) if j != i), key=lambda j: abs(j - i))[:FLAT_NEIGHBOURS]
        if near and g < FLAT_RATIO * statistics.median(scored[j][1] for j in near):
            flat.append(n)
    return flat


def score_frames(paths):
    """`paths`: {frame number: png path}. Returns per-frame black-block counts, flat frames and how many were scored."""
    frames = []
    for n in sorted(paths):
        with Image.open(paths[n]) as im:
            frames.append((n, im.convert('RGB')))
    scored = [(n, im) for n, im in frames if is_scored(im)]
    black = {n: c for n, im in scored if (c := black_blocks(im))}
    return {'black_counts': black, 'flat_frames': flat_frames(scored), 'scored': len(scored), 'total': len(frames)}
