#!/usr/bin/env python3
"""Profile of what runs under one function, from `perf script -F comm,tid,ip,sym` on stdin.

usage: foldperf.py <seconds recorded> <sample Hz> <function> [<function> ...] [-n rows]

For each function: the samples whose stack contains it, as cores, and the
symbols it called, inclusive (a symbol counts once per sample) and self.
Percentages are of the samples under that function.
"""
import collections, sys

args = sys.argv[1:]
rows = 45
if "-n" in args:
    i = args.index("-n"); rows = int(args[i + 1]); del args[i:i + 2]
secs, hz, funcs = float(args[0]), float(args[1]), args[2:]

samples = []          # each: list of symbols, innermost first
cur = None
for line in sys.stdin:
    if not line.strip():
        if cur:
            samples.append(cur)
        cur = None
    elif line[0] not in " \t":
        cur = []
    elif cur is not None:
        parts = line.split(None, 1)
        cur.append(parts[1].strip() if len(parts) > 1 else "?")
if cur:
    samples.append(cur)

print("samples=%d  process on-cpu=%.2f cores" % (len(samples), len(samples) / hz / secs))
for fn in funcs:
    incl, self_ = collections.Counter(), collections.Counter()
    n = 0
    for st in samples:
        if fn not in st:
            continue
        n += 1
        under = st[:len(st) - 1 - st[::-1].index(fn)]     # frames called from the outermost fn
        for s in set(under):
            incl[s] += 1
        self_[under[0] if under else fn] += 1
    print("\n== under %s: %d samples, %.2f cores" % (fn, n, n / hz / secs))
    if not n:
        continue
    print("-- inclusive")
    for s, c in incl.most_common(rows):
        print("%6.1f%%  %s" % (100.0 * c / n, s))
    print("-- self")
    for s, c in self_.most_common(rows // 2):
        print("%6.1f%%  %s" % (100.0 * c / n, s))
