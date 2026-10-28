"""In-launch A/B summary of a TOTK_AB run (runtime/prof_win.c rt_ab): per perf window of 60 presents wholly in an A or a
B block, the frame interval and the replay time; their means per side, and the B - A difference per A B B A cycle.

usage: ab_summary.py <log> [--from <frame>]
"""
import argparse
import re
import statistics
import sys

LINE = re.compile(r'perf: frame (\d+): interval avg ([0-9.]+) ms .*replay avg ([0-9.]+) ms; ab (\d+)/(\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='lo', type=int, default=0, help='first frame counted (after the load)')
    a = ap.parse_args()
    side = {0: [], 1: []}
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if not m or int(m.group(1)) < a.lo:
            continue
        b, n = int(m.group(4)), int(m.group(5))
        if b and b != n:
            continue   # a window across a block edge
        side[1 if b else 0].append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
    if not side[0] or not side[1]:
        print('no whole A and B windows (TOTK_AB unset, or the run too short)')
        return 1
    for k, name in ((0, 'A'), (1, 'B')):
        w = side[k]
        iv = statistics.mean(x[1] for x in w)
        rp = statistics.mean(x[2] for x in w)
        print(f'{name}: {len(w):3d} windows, interval {iv:6.2f} ms ({1000 / iv:5.1f} fps), replay {rp:5.2f} ms')
    ia, ib = statistics.mean(x[1] for x in side[0]), statistics.mean(x[1] for x in side[1])
    ra, rb = statistics.mean(x[2] for x in side[0]), statistics.mean(x[2] for x in side[1])
    print(f'B - A: interval {ib - ia:+.2f} ms ({100 * (ib - ia) / ia:+.1f}%), replay {rb - ra:+.2f} ms ({100 * (rb - ra) / ra:+.1f}%)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
