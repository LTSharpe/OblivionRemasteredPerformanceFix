"""Ranks the diagnostic TickSplit's csv (OblivionPerfFix-ticksplit.csv, written next to the DLL with HitchLedger=1 and
HitchLedgerTickSplit=1 in OblivionPerfFix.ini, both at start-up; docs/how-it-works.md "The stutter ledger").

Usage:
    py tools/tick_split.py <ticksplit.csv> [--run <bench run dir> ...] [--from-ms A --to-ms B] [--top 25] [--out x.md]

With --run, every scene of each run (result.json: t0Ms/t1Ms, QPC ms like the csv) gets its own section, built from the
2-second windows that lie inside the scene's measured interval. Without it, one section over all windows (or
--from-ms/--to-ms). Per section: the game thread's actor phase and tick groups per frame (the ledger's own timers), how
much of each group the timed tick functions explain, the actor-phase buckets outside tick groups, then the ranked
tables: by (kind, class, group), by class, by kind, the tickables, and the tick work run on other threads.
"""
import argparse
import collections
import json
import os
import sys

# ExecuteTick implementations (vtable[1] of each FTickFunction kind) in build 19115871, named from the UE 5.3 source
# and the disassembly. Unnamed kinds print as their RVA.
KINDS = {
    0x2DB9D60: "FActorTickFunction",
    0x2FDCF80: "FActorComponentTickFunction",
    0x36D8BC0: "FSkeletalMeshComponentEndPhysicsTickFunction",
    0x3578950: "FStartPhysicsTickFunction",
    0x3578620: "FEndPhysicsTickFunction",
    0x3D73820: "FNiagaraWorldManagerTickFunction",
    0x348B6F0: "ParticleSystemWorldManager tick (Cascade, per group)",
    0x491A590: "Altar tick 0x491A590 (weak target)",
    0x491A1F0: "Altar tick 0x491A1F0 (any thread)",
}
GROUPS = {0: "PrePhysics", 1: "StartPhysics", 2: "DuringPhysics", 3: "EndPhysics", 4: "PostPhysics",
          5: "PostUpdateWork", 6: "LastDemotable", 7: "NewlySpawned", 15: "outside", 16: "tickable",
          17: "timer (native delegate)", 18: "timer (dynamic delegate)", 19: "timer (TFunction)"}


def kind_label(fn, g):
    if g == 16:
        return "tickable 0x%X" % fn
    if g == 17:
        return ("timer method 0x%X" if fn < 0x6D95000 else "timer delegate vt 0x%X") % fn
    if g == 18:
        return "timer UFunction"
    if g == 19:
        return "timer TFunction 0x%X" % fn
    return kind_name(fn)
BUCKETS = ["actorstart", "setup", "startframe", "colltree", "latent", "timers", "tickables", "camera", "streaming"]


def kind_name(rva):
    return KINDS.get(rva, "0x%X" % rva)


NAMES = {}


def load_names(path):
    """An optional names.json: {pid: {"%016X" address: class name}} (class pointers the DLL could not name in that
    session, resolved by other means)."""
    if path and os.path.exists(path):
        for m in json.load(open(path)).values():
            NAMES.update(m)


def cls_name(c):
    if c.startswith("cls:"):
        return NAMES.get(c[4:].upper().rjust(16, "0"), c)
    return c


def read_csv(path):
    windows, entries, offs, tops = [], [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split(",")
            if p[0] == "W" and len(p) >= 30:
                w = {"end": float(p[1]), "sec": float(p[2]), "frames": int(p[3]), "slow": int(p[4]),
                     "period": float(p[5]), "actor": float(p[6]), "pair": float(p[7]),
                     "tg": [float(x) for x in p[8:16]], "exec_calls": int(p[16]), "exec": float(p[17]),
                     "ovf_calls": int(p[18]), "ovf": float(p[19]), "entries": int(p[20]),
                     "b": dict(zip(BUCKETS, [float(x) for x in p[21:30]]))}
                w["start"] = w["end"] - w["sec"] * 1000.0
                windows.append(w)
            elif p[0] == "E" and len(p) >= 9:
                entries.append({"end": float(p[1]), "g": int(p[2]), "a": int(p[3]), "fn": int(p[4], 16), "cls": cls_name(p[5]),
                                "calls": int(p[6]), "ms": float(p[7]), "max": float(p[8])})
            elif p[0] == "O" and len(p) >= 5:
                offs.append({"end": float(p[1]), "fn": int(p[2], 16), "calls": int(p[3]), "ms": float(p[4])})
            elif p[0] == "T" and len(p) >= 6:
                for k in range(3):
                    if len(p) > 8 + 4 * k:
                        p[8 + 4 * k] = cls_name(p[8 + 4 * k])
                tops.append(p)
    return windows, entries, offs, tops


def scenes_from_runs(runs):
    out = []
    for d in runs:
        rj = os.path.join(d, "result.json")
        if not os.path.exists(rj):
            print("no result.json in %s" % d, file=sys.stderr)
            continue
        r = json.load(open(rj, encoding="utf-8-sig"))
        for s in r.get("sessions", []):
            for sc in s.get("scenes", []) or []:
                if sc.get("t0Ms") and sc.get("t1Ms"):
                    out.append(("%s / %s" % (r.get("label", os.path.basename(d)), sc["id"]), sc["t0Ms"], sc["t1Ms"], sc))
    return out


def section(title, lo, hi, windows, entries, offs, tops, top_n, scene=None):
    ws = [w for w in windows if w["start"] >= lo - 1 and w["end"] <= hi + 1]
    if not ws:
        return ["## %s\n\nno complete 2-second window inside %.0f-%.0f ms\n" % (title, lo, hi)]
    ends = {w["end"] for w in ws}
    es = [e for e in entries if e["end"] in ends]
    os_ = [o for o in offs if o["end"] in ends]
    n = sum(w["frames"] for w in ws)
    sec = sum(w["sec"] for w in ws)
    pf = lambda v: v / n if n else 0.0
    L = ["## %s\n" % title]
    if scene:
        L.append("Bench: %s fps, Game thread busy %s ms (PresentMon cpuBusy), %s frames in the scene.\n" % (
            scene.get("fps"), scene.get("cpuBusyMs"), scene.get("frames")))
    tg = [sum(w["tg"][i] for w in ws) for i in range(8)]
    actor = sum(w["actor"] for w in ws)
    pair = sum(w["pair"] for w in ws)
    period = sum(w["period"] for w in ws)
    exec_ms = sum(w["exec"] for w in ws)
    ovf = sum(w["ovf"] for w in ws)
    bk = {b: sum(w["b"][b] for w in ws) for b in BUCKETS}
    slow = sum(w["slow"] for w in ws)
    L.append("%d windows, %.1f s, %d frames (%.1f ms per frame, %d slow >= 25 ms). Per frame: actor phase %.2f ms "
             "(pairing %.2f), tick groups %.2f ms, timed tick functions + tickables %.2f ms (table overflow %.3f).\n" % (
                 len(ws), sec, n, pf(period), slow, pf(actor), pf(pair), pf(sum(tg)), pf(exec_ms), pf(ovf)))
    # explained share per group
    by_g = collections.defaultdict(float)
    for e in es:
        by_g[e["g"]] += e["ms"]
    L.append("| group | ledger ms/frame | timed ticks ms/frame | explained |")
    L.append("|---|---:|---:|---:|")
    for i in range(8):
        if tg[i] or by_g.get(i):
            L.append("| %d %s | %.3f | %.3f | %s |" % (i, GROUPS[i], pf(tg[i]), pf(by_g.get(i, 0.0)),
                                                   "%.0f%%" % (100 * by_g.get(i, 0) / tg[i]) if tg[i] else "-"))
    for g in (15, 16, 17, 18, 19):
        if by_g.get(g):
            L.append("| %d %s | - | %.3f | - |" % (g, GROUPS[g], pf(by_g[g])))
    L.append("")
    outside = actor - sum(tg) - pair
    L.append("Actor phase outside tick groups and pairing: %.3f ms/frame; buckets (ms/frame): %s; unexplained %.3f.\n" % (
        pf(outside), ", ".join("%s %.3f" % (b, pf(bk[b])) for b in BUCKETS),
        pf(outside - sum(bk[b] for b in BUCKETS if b != "actorstart") - max(0.0, bk["actorstart"] - pair))))
    # ranked entries
    agg = collections.defaultdict(lambda: [0, 0.0, 0.0])
    for e in es:
        k = (e["fn"], e["cls"], e["g"])
        a = agg[k]
        a[0] += e["calls"]
        a[1] += e["ms"]
        a[2] = max(a[2], e["max"])
    rows = sorted(agg.items(), key=lambda kv: -kv[1][1])
    tot = exec_ms or 1.0
    L.append("### Top %d by game-thread time (kind, class, group)\n" % top_n)
    L.append("| # | kind | class | group | ms/frame | calls/frame | us/call | max ms | share of timed |")
    L.append("|---:|---|---|---|---:|---:|---:|---:|---:|")
    for i, ((fn, cls, g), (c, ms, mx)) in enumerate(rows[:top_n], 1):
        L.append("| %d | %s | %s | %s | %.3f | %.1f | %.1f | %.2f | %.1f%% |" % (
            i, kind_label(fn, g), cls, GROUPS.get(g, g), pf(ms), pf(c),
            1000 * ms / c if c else 0, mx, 100 * ms / tot))
    L.append("")
    # by class
    bc = collections.defaultdict(lambda: [0, 0.0, set()])
    for (fn, cls, g), (c, ms, mx) in agg.items():
        if g >= 16:
            continue
        bc[cls][0] += c
        bc[cls][1] += ms
        bc[cls][2].add(kind_name(fn))
    L.append("### By class (all kinds and groups)\n")
    L.append("| class | ms/frame | calls/frame | us/call | kinds |")
    L.append("|---|---:|---:|---:|---|")
    for cls, (c, ms, kinds) in sorted(bc.items(), key=lambda kv: -kv[1][1])[:top_n]:
        L.append("| %s | %.3f | %.1f | %.1f | %s |" % (cls, pf(ms), pf(c), 1000 * ms / c if c else 0, " ".join(sorted(kinds))))
    L.append("")
    bk2 = collections.defaultdict(lambda: [0, 0.0])
    for (fn, cls, g), (c, ms, mx) in agg.items():
        key = kind_name(fn) if g < 16 else ("tickables" if g == 16 else GROUPS[g])
        bk2[key][0] += c
        bk2[key][1] += ms
    L.append("### By kind\n")
    L.append("| kind | ms/frame | calls/frame |")
    L.append("|---|---:|---:|")
    for k, (c, ms) in sorted(bk2.items(), key=lambda kv: -kv[1][1]):
        L.append("| %s | %.3f | %.1f |" % (k, pf(ms), pf(c)))
    L.append("")
    if os_:
        ok = collections.defaultdict(lambda: [0, 0.0])
        for o in os_:
            ok[o["fn"]][0] += o["calls"]
            ok[o["fn"]][1] += o["ms"]
        L.append("### Tick work on other threads (per kind, not game-thread time)\n")
        L.append("| kind | ms/frame | calls/frame |")
        L.append("|---|---:|---:|")
        for fn, (c, ms) in sorted(ok.items(), key=lambda kv: -kv[1][1]):
            L.append("| %s | %.3f | %.1f |" % (kind_name(fn), pf(ms), pf(c)))
        L.append("")
    ts = [t for t in tops if lo <= float(t[2]) <= hi]
    if ts:
        L.append("### Slow frames (>= 25 ms): top entries\n")
        L.append("| frame | period ms | timed ms | 1st | 2nd | 3rd |")
        L.append("|---:|---:|---:|---|---|---|")
        for t in ts[:15]:
            cells = []
            for k in range(3):
                g, fn, cls, ms = t[6 + 4 * k:10 + 4 * k]
                cells.append("%s %s %s %s ms" % (cls, kind_name(int(fn, 16)) if fn else "", GROUPS.get(int(g), g) if g else "", ms) if fn else "")
            L.append("| %s | %s | %s | %s |" % (t[1], t[3], t[5], " | ".join(cells)))
        L.append("")
    return L


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--run", action="append", default=[])
    ap.add_argument("--from-ms", type=float)
    ap.add_argument("--to-ms", type=float)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--out")
    ap.add_argument("--names", help="an optional names.json with class names for cls:<addr> (default: names.json next to the csv, if present)")
    a = ap.parse_args()
    names = a.names or os.path.join(os.path.dirname(os.path.abspath(a.csv)), "names.json")
    if not os.path.exists(names):  # the project's own resolved names, when run from its working copy
        names = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lab", "work", "ticksplit", "names.json")
    load_names(names)
    windows, entries, offs, tops = read_csv(a.csv)
    out = ["# Tick split: %s\n" % a.csv]
    if a.run:
        for title, lo, hi, sc in scenes_from_runs(a.run):
            out += section(title, lo, hi, windows, entries, offs, tops, a.top, sc)
    else:
        lo = a.from_ms if a.from_ms is not None else float("-inf")
        hi = a.to_ms if a.to_ms is not None else float("inf")
        out += section("all windows", lo, hi, windows, entries, offs, tops, a.top)
    text = "\n".join(out) + "\n"
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text)
        print("wrote %s" % a.out)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
