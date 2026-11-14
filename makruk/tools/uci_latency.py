#!/usr/bin/env python3
"""GUI-side latency per move from a cutechess `-debug all` log (time-loss analysis).

For every normal (non-ponder) `go`: start gap = (cutechess time of the first `info depth` line
- cutechess time of `go`) - the engine's own `time` in that line. The engine's clock starts when
it reads `go`, so the gap is time the engine did not see: pipe latency, the engine process not
being scheduled, or cutechess reading its output late (e.g. a CPU-starved cutechess game thread).
A large gap is charged to the engine's clock and can flag it although the engine itself used
only a few ms.

  python makruk/tools/uci_latency.py run.log [engine-name-prefix]
Python 3 standard library only.
"""
import re
import sys
from collections import defaultdict

RX = re.compile(r"^(\d+) ([<>])(\w+\(\d+\)): (.*)$")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    prefix = argv[1] if len(argv) > 1 else ""
    st, rows = {}, []
    with open(argv[0], encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = RX.match(line)
            if not m:
                continue
            t, d, eng, msg = int(m.group(1)), m.group(2), m.group(3), m.group(4)
            if d == ">" and msg.startswith("go "):
                st[eng] = {"go": t, "ponder": " ponder " in msg + " ", "first": None, "last": 0, "msg": msg}
            elif d == "<" and eng in st:
                if msg.startswith("info depth"):
                    tm = re.search(r" time (\d+)", msg)
                    if tm:
                        if st[eng]["first"] is None:
                            st[eng]["first"] = (t, int(tm.group(1)))
                        st[eng]["last"] = int(tm.group(1))
                elif msg.startswith("bestmove"):
                    rows.append((eng, st.pop(eng), t))
    gaps, big = defaultdict(list), []
    for eng, s, t in rows:
        if s["ponder"] or s["first"] is None or not eng.startswith(prefix):
            continue
        gap = (s["first"][0] - s["go"]) - s["first"][1]
        gaps[eng.split("(")[0]].append(gap)
        if gap > 100:
            big.append((gap, t - s["go"], s["last"], eng, s["go"], s["msg"][:60]))
    for e, v in sorted(gaps.items()):
        v.sort()
        n = len(v)
        print("%s: %d normal go; start gap median %d ms, p99 %d, max %d; >50 ms %d, >100 ms %d, >300 ms %d"
              % (e, n, v[n // 2], v[int(n * 0.99)], v[-1], sum(x > 50 for x in v), sum(x > 100 for x in v),
                 sum(x > 300 for x in v)))
    for b in sorted(big, reverse=True)[:10]:
        print("  gap %d ms, wall %d ms, engine time %d ms, %s at %d: %s" % b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
