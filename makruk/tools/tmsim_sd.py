#!/usr/bin/env python3
"""Sudden-death time model: simulates one side's clock over a long game
with Platoo's TimeManagement::init, upstream formula vs the Makruk sudden-death branch.

Each move uses f * optimum (f = 1 or 2 for an unstable best move), capped at the maximum,
plus a fixed lag (GUI/OS delay). The quiet-position fast mode (B.4) starts at --quiet-from
(full move number where the last capture/Bia move happened; rule50 then grows 2 per move).

  python makruk/tools/tmsim_sd.py --base 600 --mo 300 --moves 200
Python 3 standard library only.
"""
import argparse
import math

# --- upstream (src/timeman.cpp, x basetime, inc 0, movestogo 0) -------------------------
def upstream(time_ms, ply, mo, ota):
    mtg = 50
    if time_ms < 1000:
        mtg = int(time_ms * 0.05)
    time_left = max(1, time_ms - mo * (2 + mtg))
    if ota is None:
        ota = 0.3272 * math.log10(time_left) - 0.4141
    log_t = math.log10(max(1, time_ms) / 1000.0)
    opt_c = min(0.0029869 + 0.00033554 * log_t, 0.004905)
    max_c = max(3.3744 + 3.0608 * log_t, 3.1441)
    opt_s = min(0.012112 + (ply + 3.22713) ** 0.46866 * opt_c, 0.19404 * time_ms / time_left) * ota
    max_s = min(6.873, max_c + ply / 12.352)
    opt = max(1.0, opt_s * time_left)
    mx = max(opt, min(0.8097 * time_ms - mo, max_s * opt))
    return opt, mx, ota


# --- Makruk sudden-death branch (keep in sync with src/timeman.cpp) ----------------------
H0, HMIN, HK = 100, 45, 0.5  # expected moves left: max(HMIN, H0 - HK * move)
CMAX, MAXFRAC = 2.5, 0.05            # maximum <= CMAX * timeLeft / H and <= MAXFRAC * time
Q0, Q1, QMIN = 30, 120, 0.25         # quiet mode: optimum *= max(QMIN, 1 - (rule50 - Q0) / Q1)


def sudden_death(time_ms, ply, mo, quiet_plies):
    move = ply // 2 + 1
    h = max(HMIN, H0 - HK * move)
    time_left = max(time_ms / 10.0, time_ms - mo * (2 + h))
    opt = time_left / h
    if quiet_plies > Q0:
        opt *= max(QMIN, 1.0 - (quiet_plies - Q0) / Q1)
    mx = min(CMAX * time_left / h, MAXFRAC * time_ms)
    opt = max(1.0, opt)
    return opt, max(opt, mx)


def simulate(kind, base_ms, mo, moves, f, lag, quiet_from, white=True):
    clock, ota, rows = float(base_ms), None, []
    for m in range(1, moves + 1):
        ply = 2 * (m - 1) + (0 if white else 1)
        quiet = 2 * (m - quiet_from) if quiet_from and m > quiet_from else 0
        if kind == "upstream":
            opt, mx, ota = upstream(clock, ply, mo, ota)
        else:
            opt, mx = sudden_death(clock, ply, mo, quiet)
        used = min(f * opt, mx) + lag
        clock -= used
        rows.append((m, used, clock, used / (clock + used)))
        if clock <= 0:
            break
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=float, default=600, help="base time in s (default 600 = 10+0)")
    ap.add_argument("--mo", type=float, default=300, help="Move Overhead in ms")
    ap.add_argument("--moves", type=int, default=200)
    ap.add_argument("--lag", type=float, default=30, help="real per-move delay in ms")
    ap.add_argument("--quiet-from", type=int, default=82, help="move of the last capture/Bia move (0 = never quiet)")
    a = ap.parse_args()
    marks = (20, 30, 40, 60, 100, 120, 150, 200)
    print(f"base {a.base:.0f} s, Move Overhead {a.mo:.0f} ms, lag {a.lag:.0f} ms, quiet from move {a.quiet_from}")
    print(f"{'model':10s} {'f':>3s} {'used 1-30':>9s}  " + "  ".join(f"m{m:>3d}" for m in marks)
          + "   max move>20 (%clock)  lost at")
    for kind in ("upstream", "makruk"):
        for f in (1.0, 1.5, 2.0):
            rows = simulate(kind, a.base * 1000, a.mo, a.moves, f, a.lag, a.quiet_from)
            clk = {m: c for m, _, c, _ in rows}
            used30 = sum(u for m, u, _, _ in rows if m <= 30) / 1000
            late = max((p for m, _, _, p in rows if m > 20), default=0)
            lost = next((m for m, _, c, _ in rows if c <= 0), None)
            cells = "  ".join(f"{clk[m] / 1000:4.0f}" if m in clk and clk[m] > 0 else "   -" for m in marks)
            print(f"{kind:10s} {f:3.1f} {used30:8.0f}s  {cells}   {100 * late:5.1f}%"
                  f"              {lost if lost else '-'}")


if __name__ == "__main__":
    main()
