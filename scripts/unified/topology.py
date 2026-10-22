#!/usr/bin/env python3
"""Discover sysfs topology and emit data-driven CPU policies."""
import argparse
import json
from pathlib import Path
import re

WORKER_GRID = (1, 2, 4, 8, 15)


def cpu_list(text):
    cpus = set()
    for part in text.strip().split(','):
        if part:
            bounds = [int(v) for v in part.split('-')]
            cpus.update(range(bounds[0], bounds[-1] + 1))
    return sorted(cpus)


def read(path, default=None):
    return path.read_text().strip() if path.exists() else default


def discover(sysfs_root='/sys', policy_file=None):
    root = Path(sysfs_root)
    cpu_root = root / 'devices/system/cpu'
    online = read(cpu_root / 'online')
    cpus = cpu_list(online) if online else sorted(
        int(p.name.removeprefix('cpu')) for p in cpu_root.glob('cpu[0-9]*')
        if re.fullmatch(r'cpu\d+', p.name))
    if not cpus:
        raise ValueError('no online CPUs found')
    performance = set(cpu_list(read(root / 'devices/cpu_core/cpus', '')))
    efficient = set(cpu_list(read(root / 'devices/cpu_atom/cpus', '')))
    rows = []
    for cpu in cpus:
        directory = cpu_root / f'cpu{cpu}'
        topology = directory / 'topology'
        caches = {}
        for cache in sorted((directory / 'cache').glob('index*')):
            level = read(cache / 'level')
            if level in ('2', '3'):
                caches[level] = cpu_list(read(cache / 'shared_cpu_list', str(cpu)))
        rows.append({
            'cpu': cpu,
            'package': int(read(topology / 'physical_package_id', '0')),
            'die': int(read(topology / 'die_id', '0')),
            'core': int(read(topology / 'core_id', str(cpu))),
            'cluster': int(read(topology / 'cluster_id', '-1')),
            'siblings': cpu_list(read(topology / 'thread_siblings_list', str(cpu))),
            'l2_cpus': caches.get('2', [cpu]),
            'l3_cpus': caches.get('3', [cpu]),
            'max_freq_khz': int(read(directory / 'cpufreq/cpuinfo_max_freq', '0')),
            'highest_perf': int(read(directory / 'acpi_cppc/highest_perf', '0')),
        })
    # CPPC preferred-core rankings vary on uniform AMD parts. Only the
    # kernel hybrid masks identify P/E types; retain frequency/CPPC as data.
    grouped = {}
    domains = sorted({tuple(r['l3_cpus']) for r in rows})
    for row in rows:
        cpu = row['cpu']
        if performance or efficient:
            kind = 'P' if cpu in performance else 'E' if cpu in efficient else 'uniform'
        else:
            kind = 'uniform'
        key = (row['package'], row['die'], row['core'])
        grouped.setdefault(key, []).append(dict(row, **{'class': kind}))
    cores = []
    for members in grouped.values():
        primary = min(members, key=lambda row: row['cpu'])
        cores.append({**primary, 'primary': primary['cpu'],
                      'siblings': sorted(r['cpu'] for r in members),
                      'l3_domain': domains.index(tuple(primary['l3_cpus']))})
    cores.sort(key=lambda core: core['primary'])
    policies = {'phys': physical_policy(cores)}
    if policy_file:
        extra = json.loads(Path(policy_file).read_text())
        for name, policy in extra.items():
            validate_policy(policy, cpus)
            policies[name] = policy
    domain_of = {row['cpu']: domains.index(tuple(row['l3_cpus'])) for row in rows}
    for policy in policies.values():
        policy['worker_l3_domains'] = [domain_of[cpu] for cpu in policy['worker_cpus']]
    return {'schema': 'afbench-host-profile/1', 'cpus': cpus, 'cores': cores,
            'l3_domains': [{'id': i, 'cpus': list(domain)} for i, domain in enumerate(domains)],
            'policies': policies}


def physical_policy(cores):
    primaries = [core['primary'] for core in cores]
    control = primaries.pop()
    return {'control_cpu': control, 'worker_cpus': primaries,
            'producer_cpus': list(reversed(primaries)),
            'worker_grid': [w for w in WORKER_GRID if w <= len(primaries)]}


def validate_policy(policy, cpus):
    control = policy['control_cpu']
    workers = policy['worker_cpus']
    producers = policy['producer_cpus']
    if (control not in cpus or not workers or control in workers or control in producers
            or len(set(workers)) != len(workers) or len(set(producers)) != len(producers)
            or not set(workers + producers).issubset(cpus)
            or any(w <= 0 or w > len(workers) for w in policy['worker_grid'])):
        raise ValueError('invalid CPU policy')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysfs-root', default='/sys')
    parser.add_argument('--policy-file')
    parser.add_argument('--emit', required=True, type=Path)
    args = parser.parse_args(argv)
    args.emit.write_text(json.dumps(discover(args.sysfs_root, args.policy_file), indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
