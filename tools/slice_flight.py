"""Cut mini-scenarios out of a timed `fly` scene: one short scene per problem spot, for quick iterations on that spot
alone. Each slice uses the original's waypoints, speeds, hour, weather and timescale, starts LEAD s
before its spot (so the spot's cells and NPCs attach during the timed flight, as in the full run) and ends TAIL s after
it, at full speed with no fade.

  py -3 tools/slice_flight.py --src showcase --scene rumare-loop-flight --out ic-spots \
      --spot ic-waterfront-pass:23.9:26.5:"the Waterfront channel" --spot ic-vilverin-pass:67.5:72.0:"Vilverin"

Spot times are seconds after the full flight's start (the frame plots' time axis). Times come from the waypoints'
speeds (5th value; else the scene's `speed`), as length / mean speed per segment: close to the runner's timetable
(checked on the IC flight, 2026-10-20). Check every new slice once (one or two runs) before iterating on it."""
import argparse, json, math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--src', required=True, help='source scenario name (bench/scenarios/<name>.json)')
ap.add_argument('--scene', required=True, help='fly scene id in it')
ap.add_argument('--out', required=True, help='new scenario name (written to bench/scenarios/<out>.json)')
ap.add_argument('--spot', action='append', required=True, help='id:t0:t1[:what] (seconds in the full flight)')
ap.add_argument('--lead', type=float, default=9.0, help='seconds before the spot (default 9)')
ap.add_argument('--tail', type=float, default=3.0, help='seconds after the spot (default 3)')
ap.add_argument('--settle', type=float, default=8, help='hover before the timed flight (default 8 s)')
ap.add_argument('--desc', default='', help='extra text for the scenario description')
a = ap.parse_args()

src = json.load(open(os.path.join(ROOT, 'bench', 'scenarios', a.src + '.json'), encoding='utf-8'))
s = next(x for x in src['scenes'] if x['id'] == a.scene)
if s.get('type') != 'fly' or 'path' not in s: raise SystemExit(f'{a.scene} is not a fly scene with a path')
p = s['path']
spd = lambda w: w[4] if len(w) > 4 else s['speed']
t = [0.0]
for u, v in zip(p, p[1:]): t.append(t[-1] + math.dist(u[:3], v[:3]) / ((spd(u) + spd(v)) / 2))
keep = {k: s[k] for k in ('world', 'cell', 'hour', 'timescale', 'weather', 'speed', 'pitch') if k in s}
opts = [o for o in s.get('flyopt', '').split() if o.split('=')[0] not in ('ease', 'easeout', 'fadein', 'fadeout', 'stay')]
scenes = []
for spec in a.spot:
    sid, t0, t1, *what = spec.split(':', 3)
    t0, t1 = float(t0), float(t1)
    i0 = max([i for i in range(len(p)) if t[i] <= t0 - a.lead] or [0])
    i1 = min([i for i in range(len(p)) if t[i] >= t1 + a.tail] or [len(p) - 1])
    scenes.append(dict(id=sid, type='fly', **keep, flyopt=' '.join(['ease=0', 'easeout=0'] + opts),
                       settle=a.settle, tags=sorted(set(s.get('tags', [])) - {'showcase'} | {'minispot'}),
                       comment=f'Slice of {a.src}/{a.scene}, waypoints {i0}-{i1} (full-flight seconds {t[i0]:.1f}-{t[i1]:.1f}; '
                               f'the spot is {t0}-{t1} s there, i.e. {t0 - t[i0]:.1f}-{t1 - t[i0]:.1f} s into this flight)'
                               + (f': {what[0]}.' if what else '.'),
                       path=p[i0:i1 + 1]))
out = {'name': a.out,
       'description': f'Mini-scenario: short slices of {a.src}/{a.scene} around its problem spots (tools/slice_flight.py, '
                      f'lead {a.lead:g} s, tail {a.tail:g} s). {a.desc}'.strip(),
       'settings': src.get('settings', {}), 'record': src.get('record', []), 'masks': src.get('masks', []),
       'defaults': src.get('defaults', {}), 'weathers': src.get('weathers', {}), 'scenes': scenes}
dst = os.path.join(ROOT, 'bench', 'scenarios', a.out + '.json')
json.dump(out, open(dst, 'w', encoding='utf-8', newline='\n'), indent=1)
for sc in scenes: print(f"{sc['id']}: {len(sc['path'])} waypoints, {sc['comment']}")
print('wrote', dst, '(lab only unless added to release/manifest.txt)')
