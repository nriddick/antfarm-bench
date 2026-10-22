#!/usr/bin/env python3
"""Compile the shared frame payload/driver once per variant for every adapter.

Writes OUT/kernels-{plain,audit}.o, OUT/core/libafcore{,-audit}.{a,so} and
OUT/manifest.json (hashes, compiler, flags). Workspaces receive the objects
under benchmarks/unified/microframe/payload/ and the trusted core libraries
under benchmarks/unified/core/prebuilt/ instead of kernels.c and core/src
(see microframe.py and build_core.py).
Run: python3 -B scripts/unified/build_payload.py --out DIR
"""
import argparse, hashlib, json, shutil, subprocess, sys
from pathlib import Path
import afbuild
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'benchmarks/unified/core'))
import build_core

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'benchmarks/unified/microframe'
INCLUDES = [SOURCE, ROOT / 'benchmarks/unified/core/include', ROOT / 'benchmarks/unified/core/src']


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands, objects = {}, {}
    for variant in ('plain', 'audit'):
        obj = out / f'kernels-{variant}.o'
        argv = [afbuild.CC, *afbuild.CFLAGS, *(f'-I{p}' for p in INCLUDES),
                *(['-DMF_AUDIT'] if variant == 'audit' else []), '-c', str(SOURCE / 'kernels.c'), '-o', str(obj)]
        subprocess.run(argv, cwd=ROOT, check=True)
        commands[variant] = argv
        objects[obj.name] = sha(obj)
    core = build_core.build(out / 'core')
    libraries = {p.name: sha(p) for kinds in core.values() for p in kinds.values()}
    for p in (out / 'core').glob('obj-*'):
        shutil.rmtree(p)
    (out / 'core' / 'manifest.json').write_text(json.dumps({'libraries': libraries}, indent=2) + '\n')
    sources = [SOURCE / 'kernels.c', SOURCE / 'microframe.h', *sorted((ROOT / 'benchmarks/unified/core/src').glob('*')),
               *sorted((ROOT / 'benchmarks/unified/core/include').glob('*.h'))]
    manifest = {
        'objects': objects,
        'core_libraries': libraries,
        'sources': {str(p.relative_to(ROOT)): sha(p) for p in sources},
        'commands': commands,
        'compiler': subprocess.check_output([afbuild.CC, '--version'], text=True),
        'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_status': subprocess.check_output(['git', 'status', '--short', '--', str(SOURCE), 'benchmarks/unified/core'], cwd=ROOT, text=True),
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(objects, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
