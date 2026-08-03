import csv, sys, collections
path = sys.argv[1]
rows = list(csv.DictReader(open(path)))
N = len(rows)
BTN = ("up","down","left","right","a","b","c","d","change","spell")
print(f"  rows: {N}")
for pct in (15, 40, 65, 90):
    i = N * pct // 100
    r = rows[i]
    def on(p):
        return [b for b in BTN if r.get(f"{p}_{b}") == "1"]
    print(f"    {pct:>2}% row {i}: p1={r['p1_input']:>4} {on('p1')}  p2={r['p2_input']:>4} {on('p2')}")
# activity profile: a real match has both players pressing things most of the time
act1 = sum(1 for r in rows if int(r["p1_input"] or 0))
act2 = sum(1 for r in rows if int(r["p2_input"] or 0))
print(f"  p1 active on {act1/N*100:.0f}% of frames, p2 on {act2/N*100:.0f}%")
# distinct input states -- a stuck/frozen capture would show very few
print(f"  distinct p1 states: {len({r['p1_input'] for r in rows})}, "
      f"p2: {len({r['p2_input'] for r in rows})}")
# longest run of an identical input (a frozen game would show a huge run)
def longest_run(key):
    best = cur = 1
    for a, b in zip(rows, rows[1:]):
        cur = cur + 1 if a[key] == b[key] else 1
        best = max(best, cur)
    return best
print(f"  longest identical-input run: p1={longest_run('p1_input')} frames, "
      f"p2={longest_run('p2_input')} frames  (of {N})")
