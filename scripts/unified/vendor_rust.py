#!/usr/bin/env python3
"""Copy the pinned offline Rust sources into an isolated build root."""
import sys
sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

MAIN_ROOT = Path(os.environ.get('AFBENCH_MAIN_ROOT',
                               '/home/nathan-riddick/pi_ground/antfarm_savina/open-frame-kit'))
CRATES = ('rayon-1.10.0', 'rayon-core-1.12.1', 'crossbeam-deque-0.8.6',
          'crossbeam-epoch-0.9.18', 'crossbeam-utils-0.8.21', 'either-1.13.0',
          'libc-0.2.169')
REGISTRY = 'index.crates.io-1949cf8c6b5b557f'
# Plan Q9: omit release notes containing an excluded workload reference.
EXCLUDED_FILES = (('rayon-1.10.0', 'RELEASES.md'),)


def vendor(dest):
    dest = Path(dest).resolve()
    registry = MAIN_ROOT / '.build/cargo-home/registry'
    for crate in CRATES:
        target = dest / 'vendor' / crate
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(registry / 'src' / REGISTRY / crate, target,
                        ignore=shutil.ignore_patterns('.cargo-ok'))
        for excluded_crate, relative in EXCLUDED_FILES:
            if crate == excluded_crate:
                (target / relative).unlink()
        files = {path.relative_to(target).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(target.rglob('*'))
                 if path.is_file() and path.name != '.cargo-checksum.json'}
        package = hashlib.sha256((registry / 'cache' / REGISTRY / (crate + '.crate')).read_bytes()).hexdigest()
        (target / '.cargo-checksum.json').write_text(
            json.dumps({'files': files, 'package': package}, sort_keys=True) + '\n')
    config = dest / '.cargo/config.toml'
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[source.crates-io]\nreplace-with = "vendored-sources"\n'
                      '[source.vendored-sources]\ndirectory = "vendor"\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dest', required=True, type=Path)
    args = parser.parse_args(argv)
    vendor(args.dest)


if __name__ == '__main__':
    main()
