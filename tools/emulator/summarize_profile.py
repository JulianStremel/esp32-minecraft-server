#!/usr/bin/env python3
"""Summarize ESP-IDF FreeRTOS runtime counters from emulator_profile.js.

Both counters and window duration use esp_timer microseconds. Each core's idle
percentage has one elapsed window as denominator; a whole-chip task share has
TWO elapsed windows. Unpinned task counters cannot be attributed to one core.
"""
import json
from pathlib import Path
import sys


def summarize(first, last):
    span = last['us'] - first['us']
    if span <= 0:
        raise ValueError('empty measurement window')
    before = {t['id']: t for t in first['tasks']}
    after = {t['id']: t for t in last['tasks']}
    if before.keys() - after.keys():
        raise ValueError('tasks deleted during measurement; counters would be lost')
    tasks = []
    for tid, t in after.items():
        old = before.get(tid)
        if old and old['name'] != t['name']:
            raise ValueError('task identity changed during measurement')
        delta = t['runtime_us'] - (old['runtime_us'] if old else 0)
        if delta < 0:
            raise ValueError('runtime counter regressed')
        tasks.append({'name': t['name'], 'affinity': t['affinity'], 'runtime_ms': delta / 1000,
                      'percent_one_core': 100 * delta / span, 'percent_chip': 50 * delta / span})
    # Sampling tasks/IPC create small timing skew, but a missing task or wrong
    # timer denominator must not silently produce plausible-looking percentages.
    accounted = sum(t['runtime_ms'] for t in tasks) * 1000
    if abs(accounted / (2 * span) - 1) > .01:
        raise ValueError(f'task accounting covers {100 * accounted / (2 * span):.3f}% of chip time')
    cores = []
    for cpu in (0, 1):
        idle = next(t for t in tasks if t['name'] == f'IDLE{cpu}')
        if not 0 <= idle['percent_one_core'] <= 100:
            raise ValueError('idle counter outside its measurement window')
        cores.append({'cpu': cpu, 'idle_ms': idle['runtime_ms'],
                      'busy_ms': span / 1000 - idle['runtime_ms'],
                      'idle_percent': idle['percent_one_core'],
                      'busy_percent': 100 - idle['percent_one_core']})
    return {'cores': cores, 'tasks': sorted(tasks, key=lambda t: -t['runtime_ms']),
            'accounted_percent': 100 * accounted / (2 * span)}


def main(directory):
    run = json.loads((directory / 'run.json').read_text())
    for result in run['results']:
        result.update(summarize(result['first'], result['last']))
        print(result['name'])
        for core in result['cores']:
            print(f"  CPU{core['cpu']}: busy {core['busy_percent']:.2f}%, "
                  f"idle {core['idle_percent']:.2f}% ({core['idle_ms']:.1f} ms)")
        for task in result['tasks'][:7]:
            print(f"    {task['name']:16s} {task['percent_one_core']:6.2f}% of one core")
    (directory / 'summary.json').write_text(json.dumps(run, indent=2) + '\n')


if __name__ == '__main__':
    main(Path(sys.argv[1]))
