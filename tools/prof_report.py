"""Report for a TOTK_PROFILE sampling profile (runtime/prof_win.c): top functions and the guest/runtime/outside split.

usage: prof_report.py <profile> <totk.map> [--top 40]

The map is the lld one from the link (build/<version>/totk.map); its addresses are RVAs, the profile header carries the
runtime image base, so ASLR does not matter. Symbols in the map are labels without sizes: a Rip belongs to the nearest
symbol at or below it.
"""
import argparse
import bisect
import collections
import re
import sys
from pathlib import Path

SYM = re.compile(r'^([0-9a-f]{8}) +[0-9a-f]{8} +\d+ {2,}(\S+)$')   # In column empty: object lines have a path:(section) there


def load_map(path):
    syms = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYM.match(line.rstrip('\n'))
            if m and '(' not in m.group(2):
                syms.setdefault(int(m.group(1), 16), m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def load_profile(path):
    prof = {'rips': [], 'modules': [], 'threads': [], 'samples': 0, 'base': 0}
    for line in Path(path).read_text().splitlines():
        p = line.split(None, 3)
        if not p:
            continue
        if p[0] == 'base':
            prof['base'] = int(p[1], 16)
        elif p[0] == 'samples':
            prof['samples'] = int(p[1])
        elif p[0] == 'thread':
            prof['threads'].append((int(p[1]), int(p[2]), int(p[3])))
        elif p[0] == 'module':
            prof['modules'].append((int(p[1], 16), int(p[2], 16), p[3]))
        elif p[0] == 'rip':
            prof['rips'].append((int(p[1], 16), int(p[2])))
    return prof


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('map')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    prof = load_profile(a.profile)
    addrs, names = load_map(a.map)
    base = prof['base']
    exe = next((m for m in prof['modules'] if m[0] == base), None)
    exe_end = base + exe[1] if exe else base
    total = sum(n for _, n in prof['rips']) or 1
    funcs = collections.Counter()
    cats = collections.Counter()
    for rip, n in prof['rips']:
        if base <= rip < exe_end:
            i = bisect.bisect_right(addrs, rip - base) - 1
            name = names[i] if i >= 0 else '?'
            funcs[name] += n
            cats['guest (f_*)' if name.startswith('f_') else 'runtime host (exe)'] += n
            continue
        mod = next((Path(m[2]).name for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        cats['outside: ' + (mod or 'unknown')] += n
        funcs[(mod or 'unknown') + '!+' + hex(rip - next((m[0] for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), 0))] += n
    blocked = ('outside: ntdll', 'outside: win32u', 'outside: KERNELBASE')   # waits and message loops: threads not running
    idle = sum(n for k, n in cats.items() if k.startswith(blocked))
    act = (total - idle) or 1
    print(f'samples {prof["samples"]} (Rips kept {total}), 1 ms nominal period; ntdll/win32u/KERNELBASE (blocked threads) {100 * idle / total:.1f}%')
    print('\ncategory (% of all samples, % of running samples = all minus ntdll/win32u/KERNELBASE)')
    for k, n in cats.most_common():
        print(f'  {100 * n / total:6.2f}%  {"" if k.startswith(blocked) else format(100 * n / act, "6.2f") + "%":>7}  {n:8d}  {k}')
    print(f'\ntop {a.top} functions')
    for k, n in funcs.most_common(a.top):
        print(f'  {100 * n / total:6.2f}%  {n:8d}  {k}')
    print('\nthreads (samples, % of them inside the exe)')
    for tid, n, ie in sorted(prof['threads'], key=lambda t: -t[1])[:12]:
        print(f'  tid {tid:6d} {n:8d}  {100 * ie / max(n, 1):5.1f}% in exe')


if __name__ == '__main__':
    sys.exit(main())
