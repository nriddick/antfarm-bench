#!/usr/bin/env python3
"""Build a product's agent docs bundle from pinned sources (DESIGN §5.2 item 4, Q9, Q10).

Run: python3 -B scripts/unified/mkbundle.py --system antfarm|tbb|rayon|fjp (--out DIR | --check)

The copy is deterministic: sources and globs come from
benchmarks/unified/applied/docs-bundle/manifest.json, declared redactions are
applied after their pre-SHA-256 matches and must produce their post-SHA-256,
every output file must match the manifest's expected SHA-256 list exactly,
and any match of the forbidden-workload regex fails the bundle. --out writes
the files plus BUNDLE.sha256; --check verifies without writing.
--update-manifest (maintainers only) rewrites the expected hashes from the
current sources; review its diff before committing.
"""
import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'benchmarks/unified/applied/docs-bundle/manifest.json'
MAIN_ROOT = os.environ.get('AFBENCH_MAIN_ROOT', '/home/nathan-riddick/pi_ground/antfarm_savina/open-frame-kit')
INDEX = 'BUNDLE.sha256'
# The importable Antfarm library subset that D builds vendor from the bundle.
ANTFARM_VENDOR_PREFIXES = ('antfarm/', 'threadpool/source/')


class BundleError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_manifest(path=MANIFEST):
    return json.loads(Path(path).read_text())


def _expand(text):
    return text.replace('$AFBENCH_MAIN_ROOT', MAIN_ROOT)


def _selected(relative, source):
    path = PurePosixPath(relative)
    return (any(path.full_match(pattern) for pattern in source['include'])
            and not any(path.full_match(pattern) for pattern in source.get('exclude', ())))


def _read_sources(source):
    """Yield (relative path, bytes) for every selected file of one source entry."""
    if 'zip' in source:
        with zipfile.ZipFile(_expand(source['zip'])) as archive:
            for info in archive.infolist():
                if not info.is_dir() and _selected(info.filename, source):
                    yield info.filename, archive.read(info)
        return
    root = Path(_expand(source['root']))
    if not root.is_dir():
        raise BundleError(f'source root {root} is missing')
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if path.is_file() and _selected(relative, source):
            yield relative, path.read_bytes()


def _redact(data, redaction):
    lines = data.splitlines(keepends=True)
    first, last = redaction['lines']
    if not 1 <= first <= last <= len(lines):
        raise BundleError(f'redaction {redaction["path"]}:{first}-{last} is out of range')
    return b''.join(lines[:first - 1] + lines[last:])


def collect(system, manifest=None, update=False):
    """Return {relative path: bytes} for SYSTEM's bundle, verified against the manifest.

    UPDATE recomputes the manifest's hashes instead of enforcing them.
    """
    manifest = manifest if manifest is not None else load_manifest()
    if system not in manifest['systems']:
        raise BundleError(f'no bundle definition for {system!r}')
    spec = manifest['systems'][system]
    files = {}
    for source in spec['sources']:
        for relative, data in _read_sources(source):
            if relative in files:
                raise BundleError(f'{relative} is selected by two sources')
            files[relative] = data
    if not files:
        raise BundleError(f'{system}: no files selected')
    problems = []
    for omission in spec.get('omissions', ()):
        if omission['path'] in files:
            problems.append(f'omitted file {omission["path"]} is still selected')
        source = Path(_expand(spec['sources'][0]['root'])) / omission['path']
        digest = sha256(source.read_bytes()) if source.is_file() else None
        if update:
            omission['sha256'] = digest
        elif digest != omission['sha256']:
            problems.append(f'omitted source {omission["path"]} has SHA-256 {digest}, '
                            f'manifest expects {omission["sha256"]}')
    for redaction in spec.get('redactions', ()):
        path = redaction['path']
        if path not in files:
            problems.append(f'redaction target {path} is not in the bundle')
            continue
        if update:
            redaction['pre_sha256'] = sha256(files[path])
        elif sha256(files[path]) != redaction['pre_sha256']:
            problems.append(f'redaction target {path} has pre-SHA-256 {sha256(files[path])}, '
                            f'manifest expects {redaction["pre_sha256"]}')
            continue
        files[path] = _redact(files[path], redaction)
        if update:
            redaction['post_sha256'] = sha256(files[path])
        elif sha256(files[path]) != redaction['post_sha256']:
            problems.append(f'redacted {path} has post-SHA-256 {sha256(files[path])}, '
                            f'manifest expects {redaction["post_sha256"]}')
    pattern = re.compile(manifest['forbidden_regex'].encode(), re.I)
    for relative in sorted(files):
        for number, line in enumerate(files[relative].splitlines(), 1):
            if pattern.search(line):
                problems.append(f'forbidden-workload match in {relative}:{number}')
    actual = {relative: sha256(data) for relative, data in files.items()}
    if update:
        spec['files'] = dict(sorted(actual.items()))
    else:
        expected = spec.get('files', {})
        problems += [f'unexpected file {path}' for path in sorted(set(actual) - set(expected))]
        problems += [f'missing file {path}' for path in sorted(set(expected) - set(actual))]
        problems += [f'{path}: SHA-256 {actual[path]} differs from manifest'
                     for path in sorted(set(actual) & set(expected)) if actual[path] != expected[path]]
    if problems:
        raise BundleError(f'{system} bundle failed:\n  ' + '\n  '.join(problems))
    return dict(sorted(files.items()))


def index_text(files):
    return ''.join(f'{sha256(data)}  {relative}\n' for relative, data in sorted(files.items()))


def write_files(files, out, prefixes=None):
    out = Path(out)
    for relative, data in files.items():
        if prefixes is not None and not relative.startswith(prefixes):
            continue
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o644)


def write_bundle(system, out, manifest=None):
    """Write SYSTEM's verified bundle and its BUNDLE.sha256 index into OUT."""
    files = collect(system, manifest)
    if INDEX in files:
        raise BundleError(f'source file collides with {INDEX}')
    out = Path(out)
    if out.exists() and any(out.iterdir()):
        raise BundleError(f'{out} is not empty')
    write_files(files, out)
    (out / INDEX).write_text(index_text(files))
    return files


def write_antfarm_vendor(vendor_dir, manifest=None):
    """Write the importable Antfarm subset (never the upstream root) to VENDOR_DIR/antfarm."""
    files = collect('antfarm', manifest)
    write_files(files, Path(vendor_dir) / 'antfarm', ANTFARM_VENDOR_PREFIXES)
    return Path(vendor_dir)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--system', required=True, choices=('antfarm', 'tbb', 'rayon', 'fjp'))
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--out', type=Path)
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--update-manifest', action='store_true')
    parser.add_argument('--manifest', type=Path, default=MANIFEST)
    args = parser.parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        if args.update_manifest:
            collect(args.system, manifest, update=True)
            args.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
            print(f'mkbundle: updated {args.system} hashes in {args.manifest}; review the diff')
            return 0
        if args.check:
            files = collect(args.system, manifest)
        else:
            files = write_bundle(args.system, args.out, manifest)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, BundleError) as error:
        print(f'mkbundle: {error}', file=sys.stderr)
        return 1
    print(f'mkbundle: {args.system}: {len(files)} files, BUNDLE.sha256 {sha256(index_text(files).encode())}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
