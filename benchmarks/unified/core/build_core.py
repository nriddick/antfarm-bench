#!/usr/bin/env python3
"""Build libafcore{,-audit}.{a,so}: one set of PIC objects per variant, both
library kinds from the same objects. Output is byte-reproducible across
checkout paths (relative sources, prefix map, deterministic archives).

A core directory holding prebuilt/manifest.json (benchmark workspaces, which
do not receive src/) installs those hash-checked libraries instead.

Run: python3 -B benchmarks/unified/core/build_core.py [--out DIR] [--variant plain|audit|all]
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


CORE = Path(__file__).resolve().parent
ROOT = CORE.parents[2]

sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts' / 'unified'))
from afbuild import CC, AR, AR_FLAGS, CFLAGS, CORE_LINK_FLAGS, AUDIT_DEFINES
SOURCES = ['src/json.c', 'src/topology_cpus.c', 'src/registry.c',
           'src/gol.c', 'src/services.c']
VARIANTS = {
    'plain': {'name': 'afcore', 'defines': [], 'sources': SOURCES},
    'audit': {'name': 'afcore-audit', 'defines': AUDIT_DEFINES,
              'sources': SOURCES + ['src/audit.c']},
}


def run(command, cwd):
    completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError('command failed ({}): {}\n{}{}'.format(
            completed.returncode, shlex.join(command), completed.stdout,
            completed.stderr))


def install_prebuilt(prebuilt, out_dir, variants):
    """Copy hash-checked prebuilt libraries into out_dir; same result shape as build."""
    import hashlib, json, shutil
    manifest = json.loads((prebuilt / 'manifest.json').read_text())
    out_dir.mkdir(parents=True, exist_ok=True)
    built = {}
    for variant in variants:
        built[variant] = {}
        for kind in ('a', 'so'):
            name = 'lib{}.{}'.format(VARIANTS[variant]['name'], kind)
            if hashlib.sha256((prebuilt / name).read_bytes()).hexdigest() != manifest['libraries'][name]:
                raise RuntimeError('prebuilt {} does not match its manifest'.format(name))
            shutil.copy2(prebuilt / name, out_dir / name)
            built[variant][kind] = out_dir / name
    return built


def build(out_dir, variants=('plain', 'audit'), extra_cflags=(), core=CORE):
    """Build the requested variants into out_dir; return {variant: {kind: path}}."""
    out_dir = Path(out_dir).resolve()
    core = Path(core).resolve()
    if (core / 'prebuilt' / 'manifest.json').exists() and not extra_cflags:
        return install_prebuilt(core / 'prebuilt', out_dir, variants)
    root = core.parents[2]
    flags = CFLAGS + ['-ffile-prefix-map={}=.'.format(root),
                      '-ffile-prefix-map={}=.'.format(out_dir),
                      '-I', 'include', '-I', 'src', *extra_cflags]
    built = {}
    for variant in variants:
        spec = VARIANTS[variant]
        obj_dir = out_dir / ('obj-' + variant)
        obj_dir.mkdir(parents=True, exist_ok=True)
        objects = []
        for source in spec['sources']:
            obj = obj_dir / (Path(source).stem + '.o')
            run([CC, *flags, *spec['defines'], '-c', source,
                 '-o', os.path.relpath(obj, core)], core)
            objects.append(os.path.relpath(obj, out_dir))
        archive = 'lib{}.a'.format(spec['name'])
        shared = 'lib{}.so'.format(spec['name'])
        (out_dir / archive).unlink(missing_ok=True)
        run([AR, *AR_FLAGS, archive, *objects], out_dir)
        run([CC, *CORE_LINK_FLAGS, *extra_cflags,
             '-Wl,-soname,' + shared,
             '-o', shared, *objects], out_dir)
        built[variant] = {'a': out_dir / archive, 'so': out_dir / shared}
    return built


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', default=str(ROOT / '.build' / 'unified' / 'core'))
    parser.add_argument('--variant', choices=['plain', 'audit', 'all'], default='all')
    args = parser.parse_args(argv)
    variants = ('plain', 'audit') if args.variant == 'all' else (args.variant,)
    try:
        built = build(args.out, variants)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    for paths in built.values():
        for path in paths.values():
            print(path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
