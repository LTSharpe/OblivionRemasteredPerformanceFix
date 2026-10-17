#!/usr/bin/env python3
"""Stutter ledger: ranks the slow game-thread frames of a play session by cause.

Input: the DLL diagnostic HitchLedger's OblivionPerfFix-ledger.csv ([Fixes] HitchLedger=1), given as
  - a capture folder (captures\\play\\<stamp>-<label>, which Record-Session.ps1 fills), or
  - the game's Win64 folder, or
  - the csv itself.
PresentMon's frames.csv next to it (Record-Session) is joined by QPC time to show what reached the screen.

Output: ledger.md next to the input (or --out): focused in-world play time; slow frames and excess ms per hour by
cause; menus, transition edges and loading screens in their own buckets; the 20 worst frames; the unlabelled share.

    python tools/stutter_ledger.py captures/play/20261016-200000-play
    python tools/stutter_ledger.py "C:/.../Win64" --from-ms 123456 --to-ms 234567 --out ledger-part.md
"""
import argparse
import csv
import io
import math
import os
import sys
import time
from collections import defaultdict

TG_NAMES = ["TG_PrePhysics", "TG_StartPhysics", "TG_DuringPhysics", "TG_EndPhysics", "TG_PostPhysics",
            "TG_PostUpdateWork", "TG_LastDemotable", "TG_NewlySpawned"]
F_FADE, F_GRID, F_UNFOCUSED, F_TRAVEL, F_PSO = 4, 8, 16, 32, 64
EDGE_FRAMES = 2          # a state change this many frames ago (or less) makes the frame a transition edge
AFTER_LOAD_FRAMES = 300  # frames after the fade-to-game counted as "after a loading screen" (about 5 s at 60 fps)
DOMINANT = 0.5           # a stage holding at least this share of the excess names the cause
GRID_MS = 1.0            # the OT's grid transition this long (ms) in the frame = a cell-row crossing


def find_csv(path):
    if os.path.isfile(path):
        return path
    for name in ("OblivionPerfFix-ledger.csv",):
        p = os.path.join(path, name)
        if os.path.isfile(p):
            return p
    sys.exit(f"no OblivionPerfFix-ledger.csv in {path}")


def read_ledger(path, t0=None, t1=None):
    meta, fcols, scols, frames, sums = {}, None, None, [], []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("# OblivionPerfFix-ledger"):
                for part in line[2:].split(", "):
                    if "=" in part:
                        k, v = part.split("=", 1)
                        meta[k.strip()] = v.strip()
                    elif part.startswith("OblivionPerfFix "):
                        meta["version"] = part.split(" ", 1)[1]
                frames.append({"_session": True})
                continue
            if line.startswith("#F,"):
                fcols = line[1:].split(",")
                continue
            if line.startswith("#S,"):
                scols = line[1:].split(",")
                continue
            if line.startswith("#"):
                continue
            row = line.split(",")
            if row[0] == "F" and fcols and len(row) == len(fcols):
                d = {}
                for k, v in zip(fcols[1:], row[1:]):
                    if k == "state":
                        d[k] = v
                    else:
                        try:
                            d[k] = float(v)
                        except ValueError:
                            d[k] = math.nan
                if (t0 is None or d["end_ms"] >= t0) and (t1 is None or d["end_ms"] <= t1):
                    frames.append(d)
            elif row[0] == "S" and scols and len(row) == len(scols):
                d = {k: float(v) for k, v in zip(scols[1:], row[1:])}
                if (t0 is None or d["end_ms"] >= t0) and (t1 is None or d["end_ms"] <= t1):
                    sums.append(d)
    frames = [f for f in frames if "_session" not in f]
    return meta, frames, sums


def read_presentmon(folder, explicit=None):
    p = explicit or os.path.join(folder, "frames.csv")
    if not explicit and not os.path.isfile(p):
        p = os.path.join(folder, "perf-frames.csv")  # a benchmark run's PresentMon file
    if not os.path.isfile(p):
        return None
    out = []
    with open(p, encoding="utf-8-sig", errors="replace") as fh:
        r = csv.DictReader(fh)
        for row in r:
            try:
                t = float(row.get("CPUStartQPCTimeInMs") or "nan")
                gap = row.get("MsBetweenDisplayChange") or "NA"
                out.append((t, float(gap) if gap not in ("NA", "") else math.nan,
                            float(row.get("MsBetweenPresents") or "nan")))
            except ValueError:
                continue
    out.sort()
    return out


def screen_gap(pm, start_ms, end_ms):
    """Largest display-change gap among presents whose CPU start falls in the frame (+ one frame after)."""
    if not pm:
        return math.nan
    import bisect
    i = bisect.bisect_left(pm, (start_ms,))
    best = math.nan
    while i < len(pm) and pm[i][0] <= end_ms + 34:
        g = pm[i][1]
        if not math.isnan(g) and (math.isnan(best) or g > best):
            best = g
        i += 1
    return best


def stages(f):
    """Candidate stages (ms) for a frame. Labelled stages first, then the unlabelled phase residuals."""
    period = f["period_ms"]
    tg = [f.get(f"tg{i}_ms", 0.0) for i in range(8)]
    sync = f["sync_ms"] if f["sync_ms"] >= 0 else 0.0
    bodies = min(f["bodies_ms"], period)
    pair, unit, flow = f["pair_ms"], f["unit_ms"], f["flow_ms"]
    gpu = f["gpu_ms"]
    st = {}
    st["physics arrival"] = tg[3] + tg[1] + bodies
    if f["grid_ms"] >= GRID_MS:  # the OT's grid function runs every OT frame; a crossing takes milliseconds
        st["cell-row crossing"] = pair
    else:
        st["old-engine wait"] = pair
    st["actor arrival"] = unit + flow
    pso = (f["pso_ms"] > 0.5) or (int(f["flags"]) & F_PSO)
    rt = "GPU bound" if gpu > 0 and gpu >= 0.85 * period else "render-thread bound"
    st[rt + (" (PSO precompile)" if pso else "")] = sync
    if f["fade_ms"] > 0:
        st["fade-to-game call"] = f["fade_ms"]
    other_tg = [(tg[i], i) for i in (0, 2, 4, 5, 6, 7)]
    tick = sum(v for v, _ in other_tg)
    top = max(other_tg)[1]
    st[f"tick work ({TG_NAMES[top]} most)"] = tick
    # unlabelled phase residuals
    actor_rest = f["actor_ms"] - sum(tg) - pair - unit - flow - max(0.0, f["fade_ms"])  # the fade-to-game runs in the actor phase
    st["unlabelled: before the world tick (async loading, travel, streaming)"] = f["pre_ms"]
    st["unlabelled: actor phase outside tick groups"] = max(0.0, actor_rest)
    st["unlabelled: frame tail (GC, UI, draw, level streaming)"] = max(0.0, f["post_ms"] - sync)
    return st


def classify(f):
    cap = f["cap_ms"] if f["cap_ms"] > 1 else 16.667
    excess = max(0.0, f["period_ms"] - cap)
    st = stages(f)
    name, ms = max(st.items(), key=lambda kv: kv[1])
    if excess <= 0:
        return "none", excess, st
    if ms >= DOMINANT * excess:
        return name, excess, st
    return "mixed", excess, st


def bucket(f, ignore_focus):
    flags = int(f["flags"])
    if f["state"] == "loading" or flags & F_TRAVEL:
        return "loading"
    if f["since_change"] <= EDGE_FRAMES or flags & F_FADE:
        return "edge"
    if not ignore_focus and flags & F_UNFOCUSED:
        return "unfocused"
    if f["state"] == "menu":
        return "menu"
    if f["state"] == "play":
        return "play"
    return "other"


def fmt(v, nd=1):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    return f"{v:.{nd}f}"


def place(f):
    if not (f.get("x") or f.get("y")):
        return "-"
    return f"{f['x']:.0f}, {f['y']:.0f}, {f['z']:.0f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--out")
    ap.add_argument("--from-ms", type=float)
    ap.add_argument("--to-ms", type=float)
    ap.add_argument("--ignore-focus", action="store_true", help="count unfocused play as play (hidden benchmark games)")
    ap.add_argument("--title", default="")
    ap.add_argument("--presentmon", help="PresentMon csv to join (default: frames.csv or perf-frames.csv next to the input)")
    a = ap.parse_args()
    csv_path = find_csv(a.path)
    folder = a.path if os.path.isdir(a.path) else os.path.dirname(os.path.abspath(a.path))
    meta, frames, sums = read_ledger(csv_path, a.from_ms, a.to_ms)
    pm = read_presentmon(folder, a.presentmon)

    # denominators
    tot = defaultdict(float)
    hist = defaultdict(lambda: [0] * 6)
    for s in sums:
        for st in ("play", "menu", "loading", "other"):
            tot[st + "_s"] += s[st + "_s"]
            tot[st + "_n"] += s[st + "_n"]
            tot[st + "_slow"] += s[st + "_slow"]
            tot[st + "_excess"] += s[st + "_excess_ms"]
            for b in range(6):
                hist[st][b] += int(s[f"{st}_h{b}"])
        tot["unfocused_play_s"] += s["unfocused_play_s"]
        tot["dropped"] += s["dropped"]
    ignore_focus = a.ignore_focus
    focus_note = ""
    if not ignore_focus and tot["play_s"] > 0 and tot["unfocused_play_s"] >= 0.98 * tot["play_s"]:
        ignore_focus = True
        focus_note = ("The game never had the keyboard focus (a hidden benchmark game): unfocused play is counted as "
                      "play.")
    play_s = tot["play_s"] - (0 if ignore_focus else tot["unfocused_play_s"])
    play_h = play_s / 3600.0

    # classify
    rows = []
    for f in frames:
        b = bucket(f, ignore_focus)
        cause, excess, st = classify(f)
        rows.append((f, b, cause, excess, st))

    by = defaultdict(lambda: defaultdict(lambda: [0, 0.0]))
    for f, b, cause, excess, st in rows:
        e = by[b][cause]
        e[0] += 1
        e[1] += excess

    out = io.StringIO()
    w = out.write
    w(f"# Stutter ledger{(': ' + a.title) if a.title else ''}\n\n")
    w(f"Source: `{csv_path}`" + (f" (from {a.from_ms:.0f} to {a.to_ms:.0f} ms)" if a.from_ms or a.to_ms else "") + "\n\n")
    w(f"DLL {meta.get('version', '?')}, threshold {meta.get('threshold_ms', '?')} ms, signals {meta.get('signals', '?')}.")
    if pm is not None:
        w(f" PresentMon frames joined: {len(pm)}.")
    w("\n\n")
    if focus_note:
        w(focus_note + "\n\n")
    if tot["dropped"]:
        w(f"**{tot['dropped']:.0f} records were dropped** (ring full): the counts below are a lower bound.\n\n")

    w("## Time and frames by state\n\n")
    w("| State | time, s | frames | slow frames | excess ms (DLL) | <16.7 | <25 | <33 | <50 | <100 | >=100 |\n")
    w("|---|---|---|---|---|---|---|---|---|---|---|\n")
    for st in ("play", "menu", "loading", "other"):
        h = hist[st]
        w(f"| {st} | {tot[st + '_s']:.0f} | {tot[st + '_n']:.0f} | {tot[st + '_slow']:.0f} | {tot[st + '_excess']:.0f} | "
          + " | ".join(str(x) for x in h) + " |\n")
    w(f"\nFocused in-world play: **{play_s / 60:.1f} min**"
      + ("" if ignore_focus else f" (unfocused play {tot['unfocused_play_s'] / 60:.1f} min left out)") + ".\n\n")

    def table(bname, title, per_hour):
        causes = by.get(bname)
        if not causes:
            w(f"## {title}\n\nNo slow frames.\n\n")
            return
        total_ex = sum(v[1] for v in causes.values())
        w(f"## {title}\n\n")
        if per_hour and play_h > 0:
            w("| Cause | frames | excess ms | excess ms / hour | share |\n|---|---|---|---|---|\n")
        else:
            w("| Cause | frames | excess ms | share |\n|---|---|---|---|\n")
        for cause, (n, ex) in sorted(causes.items(), key=lambda kv: -kv[1][1]):
            share = 100 * ex / total_ex if total_ex else 0
            if per_hour and play_h > 0:
                w(f"| {cause} | {n} | {ex:.0f} | {ex / play_h:.0f} | {share:.0f}% |\n")
            else:
                w(f"| {cause} | {n} | {ex:.0f} | {share:.0f}% |\n")
        unl = sum(v[1] for k, v in causes.items() if k.startswith("unlabelled") or k == "mixed")
        tick = sum(v[1] for k, v in causes.items() if k.startswith("tick work"))
        w(f"\nUnlabelled (mixed or an unlabelled phase): **{100 * unl / total_ex:.0f}%** of the excess; tick work "
          f"(game-thread time inside tick groups, no finer label): {100 * tick / total_ex:.0f}%.\n\n")

    table("play", "In-world play: slow frames by cause", True)
    after = [r for r in rows if r[1] == "play" and r[0]["since_fade"] < AFTER_LOAD_FRAMES]
    if after:
        ex = sum(r[3] for r in after)
        w(f"Of these, {len(after)} frames ({ex:.0f} ms excess) came within {AFTER_LOAD_FRAMES} frames after a loading "
          "screen's fade-to-game.\n\n")
    table("menu", "Menus (inventory, dialogue, map, pause): slow frames by cause", False)
    table("edge", "Transition edges (a menu opens or closes, the fade-to-game frame)", False)
    table("unfocused", "Play without the keyboard focus", False)
    table("other", "Other states", False)
    nload = sum(1 for r in rows if r[1] == "loading")
    w(f"## Loading screens\n\n{nload} slow frames behind loading screens (not ranked: the player does not see them).\n\n")

    # stage totals for play (where the excess goes on average)
    play_rows = [r for r in rows if r[1] == "play"]
    if play_rows:
        agg = defaultdict(float)
        for f, b, cause, excess, st in play_rows:
            for k, v in st.items():
                agg[k.split(" (")[0] if k.startswith("tick work") else k] += v
        w("## In-world play: stage totals over the slow frames (ms, stages overlap)\n\n| Stage | ms |\n|---|---|\n")
        for k, v in sorted(agg.items(), key=lambda kv: -kv[1]):
            w(f"| {k} | {v:.0f} |\n")
        w("\n")

    worst = sorted([r for r in rows if r[1] != "loading"], key=lambda r: -r[0]["period_ms"])[:20]
    w("## The 20 worst frames (loading screens left out)\n\n")
    w("| end, s | period | bucket | cause | place (UE x, y, z) | pre | actor | post | sync | tg EndPhys | pair | grid | "
      "units+flow | bodies | GPU | screen gap |\n")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    t_first = frames[0]["end_ms"] if frames else 0
    for f, b, cause, excess, st in worst:
        gap = screen_gap(pm, f["end_ms"] - f["period_ms"], f["end_ms"]) if pm else math.nan
        w(f"| {(f['end_ms'] - t_first) / 1000:.1f} | {f['period_ms']:.1f} | {b} | {cause} | {place(f)} | {fmt(f['pre_ms'])} | "
          f"{fmt(f['actor_ms'])} | {fmt(f['post_ms'])} | {fmt(f['sync_ms'])} | {fmt(f['tg3_ms'])} | {fmt(f['pair_ms'])} | "
          f"{fmt(f['grid_ms'])} | {fmt(f['unit_ms'] + f['flow_ms'])} | {fmt(f['bodies_ms'])} | {fmt(f['gpu_ms'])} | {fmt(gap)} |\n")
    w("\nTimes in ms; `end, s` from the first slow frame of the file. Causes: the stage holding >= 50% of the frame's "
      "excess (period - cap period, 16.7 ms without a cap), else `mixed`; rules at the top of tools/stutter_ledger.py.\n")

    target = a.out or os.path.join(folder, "ledger.md")
    if not a.out and os.path.exists(os.path.join(folder, "OblivionRemastered-Win64-Shipping.exe")):
        # never write into the game folder: the report goes to captures\play\<stamp>-steam\ instead
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        folder = os.path.join(root, "captures", "play", time.strftime("%Y%m%d-%H%M%S") + "-steam")
        os.makedirs(folder, exist_ok=True)
        target = os.path.join(folder, "ledger.md")
    with open(target, "w", encoding="utf-8") as fh:
        fh.write(out.getvalue())
    print(f"ledger: {len(frames)} slow frames, {play_s / 60:.1f} min play -> {target}")


if __name__ == "__main__":
    main()
