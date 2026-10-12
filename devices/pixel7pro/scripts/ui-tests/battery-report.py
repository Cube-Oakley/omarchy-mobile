#!/usr/bin/env python3
"""Summary of a battery-run.py log (test only; runs anywhere).

  battery-report.py battery-run.jsonl [--from START_EPOCH]

Charge used and the projected battery life, the current by activity and by
rest, how much of each rest the phone slept, whether the wake requests came
on time, failures, kernel warnings and the owner's interruptions.
"""
import json
import sys
from collections import defaultdict

ACTIVITIES = ('x', 'video', 'web', 'apps', 'social', 'android', 'camera', 'idle')


def main(path, since=0):
    entries = [json.loads(line) for line in open(path) if line.strip()]
    entries = [e for e in entries if e['t'] >= since]
    starts = [i for i, e in enumerate(entries) if e['ev'] == 'start']
    if not starts:
        sys.exit('no start in the log')
    entries = entries[starts[-1]:]
    first, last = entries[0], entries[-1]
    hours = (last['t'] - first['t']) / 3600
    used = last['used']
    drop = first['cap'] - last['cap']
    print(f"run: {entries[0].get('profile')} from {first['cap']}% to {last['cap']}% over {hours:.2f} h, "
          f"{used:.0f} mAh ({used / max(hours, 1e-3):.0f} mA average)")
    if drop > 0:
        print(f'projected: {hours / drop * 100:.1f} h from 100 to 0%, {hours / drop * 90:.1f} h from 100 to 10%')
    by = defaultdict(lambda: [0, 0.0, 0.0, 0])  # count, seconds, mAh, fails
    for e in entries:
        if e['ev'] in ACTIVITIES:
            b = by[e['ev']]
            b[0] += 1
            b[1] += e.get('s', 0)
            b[2] += e.get('mah', 0)
            b[3] += 0 if e.get('ok') else 1
    on_seconds = sum(b[1] for b in by.values())
    print(f'\nscreen in use: {on_seconds / 3600:.2f} h ({100 * on_seconds / max(1, hours * 3600):.0f}% of the run)')
    print(f"{'activity':10} {'n':>4} {'min':>7} {'mAh':>7} {'mA':>6} {'fails':>5}")
    for name in ACTIVITIES:
        if name in by:
            n, s, mah, fails = by[name]
            print(f'{name:10} {n:4} {s / 60:7.1f} {mah:7.1f} {mah * 3600 / max(s, 1):6.0f} {fails:5}')
    rests = defaultdict(lambda: [0, 0.0, 0.0, 0.0, [], 0, 0])  # n, min, mAh, asleep min, late, suspends, fails
    for e in entries:
        if e['ev'].endswith('-end') and 'mode' in e:
            r = rests[(e['ev'][:-4], e['mode'])]
            r[0] += 1
            r[1] += e['minutes']
            r[2] += e['mah']
            r[3] += e['minutes'] * e.get('asleep_pct', 0) / 100
            r[4].append(e.get('late', 0))
            r[5] += e.get('suspends', 0)
            r[6] += e.get('suspend_fails', 0)
    print(f"\n{'rest':14} {'n':>4} {'min':>7} {'mAh':>7} {'mA':>6} {'asleep':>6} {'late s (max)':>13} {'suspends':>8}")
    for (why, mode), (n, minutes, mah, asleep, late, sus, fails) in sorted(rests.items()):
        print(f'{why + " " + mode:14} {n:4} {minutes:7.1f} {mah:7.1f} {mah * 60 / max(minutes, 1e-3):6.0f} '
              f'{100 * asleep / max(minutes, 1e-3):5.0f}% {sum(late) / n:6.1f} ({max(late):5.1f}) {sus:5}+{fails}f')
    wakes = defaultdict(int)
    problems = []
    for e in entries:
        for k, v in e.get('wakes', {}).items():
            wakes[k] += v
        problems += e.get('problems', [])
    print('\nwakes in rests:', dict(wakes))
    if problems:
        print('sleep problems:', len(problems))
        for p in problems[-8:]:
            print('   ', p)
    bursts = [e for e in entries if e['ev'] == 'burst-end']
    print(f"bursts: {len(bursts)}, {sum(e.get('cycles', 0) for e in bursts)} wake/sleep cycles")
    users = [e for e in entries if e['ev'] == 'user']
    print(f'owner interruptions: {len(users)}', [e.get('why') for e in users][-5:])
    kernel = sum(e.get('kernel', 0) for e in entries)
    print(f'kernel warnings: {kernel}')
    seen = defaultdict(int)
    for e in entries:
        for line in e.get('kernel_first', []):
            seen[line.split(':')[0][:60]] += 1
    for line, n in sorted(seen.items(), key=lambda kv: -kv[1])[:10]:
        print(f'   {n:4} {line}')
    relights = [e for e in entries if e['ev'] == 'relight']
    if relights:
        print(f"screen on again while the phone got ready to sleep: {len(relights)}, "
              f"Wi-Fi back in {[e.get('wifi_s') for e in relights]} s")
    down = [e for e in entries if e['ev'] == 'wifi-down']
    restarts = [e for e in entries if e['ev'] == 'wifi-restart']
    print(f"Wi-Fi down after a wake: {len(down)}, restarts that brought it back: "
          f"{sum(1 for e in restarts if e.get('back'))}/{len(restarts)}")
    for e in down[-3:]:
        for line in e.get('driver', [])[-6:]:
            print('   ', line[:160])
    for e in entries:
        if e['ev'] in ACTIVITIES and e.get('ok') is None:
            print('interrupted', e['ev'], e.get('interrupted', ''), 'touch irqs', e.get('touch_irqs'))
        elif e['ev'] in ('error', 'recover-failed', 'burst-fail') or (e['ev'] in ACTIVITIES and not e.get('ok')):
            print('FAIL', e['ev'], e.get('error', '')[:200])
    temps = [e['temp'] for e in entries if 'temp' in e]
    print(f'battery temperature: {min(temps)}-{max(temps)} C')
    if last['ev'] == 'finish':
        print('finished:', last.get('why'))


if __name__ == '__main__':
    since = float(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0
    main(sys.argv[1], since)
