"""Condense the TOTK_TRACE=perf and TOTK_TRACE=sched lines of a game log over a frame window.

    python tools/perf_summary.py build/v121/logs/fight.log 1560 2600
"""
import re
import statistics
import sys

PERF = re.compile(r'perf: frame (\d+): interval avg ([\d.]+) ms max ([\d.]+) ms \(([\d.]+) fps\); render thread blocked: '
                  r'pacing (\d+)%.*?pipeline build (\d+)%.*?texture (\d+)%')
GPU = re.compile(r'perf: frame (\d+): gpu avg (?:([\d.]+) ms|n/a).*?fence wait per frame: render thread ([\d.]+) ms, '
                 r'other threads ([\d.]+) ms; vram (?:([\d.]+) of ([\d.]+) MiB|n/a).*?ram working set ([\d.]+) MiB, '
                 r'private ([\d.]+) MiB; bound: (\w+)')
SCHED = re.compile(r'sched: frame (\d+): (.*)')
THREAD = re.compile(r'sched: (render )?(0x[0-9a-f]+) (p\d+ m\d+) run (\d+)% core (\d+)% host (\d+)% sleep (\d+)% '
                    r'sync (\d+)% cv (\d+)% lock (\d+)% addr (\d+)% cpu (\d+)% blocks (\d+) preempted (\d+)')
RWAIT = re.compile(r'sched: render wait (\d): (.*)')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1560
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    fr, fps, mx, gpu, vram, ram, bound = None, [], [], [], [], [], {}
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        if line.startswith('run_game:'):
            print(line.rstrip())
        elif m := PERF.search(line):
            f = int(m[1])
            if lo < f <= hi:
                fps.append(float(m[4]))
                mx.append(float(m[3]))
                print(f'perf {f}: {m[4]} fps max {m[3]} pacing {m[5]}% pipe {m[6]}% tex {m[7]}%')
        elif m := GPU.search(line):
            f = int(m[1])
            if lo < f <= hi:
                if m[2]:
                    gpu.append(float(m[2]))
                if m[5]:
                    vram.append((float(m[5]), float(m[6])))
                ram.append((float(m[7]), float(m[8])))
                bound[m[9]] = bound.get(m[9], 0) + 1
                print(f'  gpu {f}: {m[2] or "n/a"} ms, fence wait render {m[3]} other {m[4]} ms, '
                      f'vram {m[5] or "n/a"} MiB, ram ws {m[7]} private {m[8]} MiB, bound {m[9]}')
        elif m := SCHED.search(line):
            fr = int(m[1])
            if lo < fr <= hi:
                print(f'  sched {fr}: {m[2]}')
        elif (m := THREAD.search(line)) and fr and lo < fr <= hi:
            print(f'    {m[1] or ""}{m[2]} {m[3]} run {m[4]} core {m[5]} host {m[6]} sleep {m[7]} sync {m[8]} cv {m[9]} '
                  f'lock {m[10]} addr {m[11]} cpu {m[12]} blk {m[13]} pre {m[14]}')
        elif (m := RWAIT.search(line)) and fr and lo < fr <= hi:
            print(f'    rw{m[1]}: {m[2]}')
    if fps:
        print(f'SUMMARY frames {lo}-{hi}: mean fps {statistics.mean(fps):.1f}, min window fps {min(fps):.1f}, '
              f'max frame {max(mx):.1f} ms')
    if ram:
        g = f'gpu mean {statistics.mean(gpu):.2f} ms' if gpu else 'gpu n/a'
        v = f'vram max {max(u for u, _ in vram):.0f} of {vram[-1][1]:.0f} MiB' if vram else 'vram n/a'
        b = ', '.join(f'{k} {n}' for k, n in sorted(bound.items()))
        print(f'SUMMARY gpu: {g}, {v}, ram ws max {max(w for w, _ in ram):.0f} MiB, '
              f'private max {max(p for _, p in ram):.0f} MiB; bound windows: {b}')


if __name__ == '__main__':
    main()
