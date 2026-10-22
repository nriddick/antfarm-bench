#!/usr/bin/env python3
"""Canonical build flags and recipes for repository and isolated workspace use."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

CC = 'clang'
CXX = 'clang++'
AR = 'llvm-ar'
AR_FLAGS = ['rcsD']
CFLAGS = ['-O2', '-DNDEBUG', '-fPIC', '-std=c11', '-pthread',
          '-fvisibility=hidden', '-Wall', '-Wextra', '-Werror']
AUDIT_DEFINES = ['-DAF_AUDIT']
CORE_LINK_FLAGS = ['-shared', '-pthread', '-Wl,--build-id=sha1']
CPP_FLAGS = ['-std=c++20', '-O2', '-DNDEBUG']
D_FLAGS = ['-O2', '-release', '-i']
RUST_FLAGS = ['build', '--release', '--locked', '--offline']
JAVA_FLAGS = ['--release', '25']
JVM_FLAGS = ['-XX:+UseG1GC', '-Xms2g', '-Xmx2g', '-XX:+AlwaysPreTouch',
             '--enable-native-access=ALL-UNNAMED', '-XX:-UsePerfData']
# Every JVM (javac, version probes, runs) skips /tmp/hsperfdata_*; javac and runs use
# impl.parent / '.build/tmp' (the build root's tmp directory) as java.io.tmpdir.
JVM_PROBE_FLAGS = ['-XX:-UsePerfData']
# W-D2 must use this package identity and a lock for this harness manifest.
RUST_PACKAGE = 'afbench-impl'
RUST_BINARY = 'benchmark'
RAYON_VERSION = '1.10.0'
RUST_LOCK = 'benchmarks/unified/bindings/rust/afcore-sys/stub/Cargo.lock'
RUST_VENDOR_HELPER = 'scripts/unified/vendor_rust.py'

MAIN_ROOT = Path(os.environ.get('AFBENCH_MAIN_ROOT',
                               '/home/nathan-riddick/pi_ground/antfarm_savina/open-frame-kit'))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def system_version(system, *, tbb_header=None, tbb_library=None, jdk=None):
    if system == 'c':
        return 'c'
    if system == 'rayon':
        return RAYON_VERSION
    if system == 'antfarm':
        # Repository callers supply the trusted docs/upstreams.txt identity.
        return None
    if system == 'tbb':
        header = Path(tbb_header) if tbb_header else Path('/usr/include/oneapi/tbb/version.h')
        library = Path(tbb_library) if tbb_library else Path('/usr/lib/libtbb.so.12')
        defines = dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)', header.read_text(), re.M))
        version = '.'.join(defines['TBB_VERSION_' + part].strip()
                           for part in ('MAJOR', 'MINOR', 'PATCH'))
        suffix = defines.get('__TBB_VERSION_SUFFIX', '""').strip().strip('"')
        return (f'{version}{suffix}; interface {defines["TBB_INTERFACE_VERSION"].strip()}; '
                f'libtbb.so.12 sha256 {digest(library)}')
    if system == 'fjp':
        jdk = Path(jdk) if jdk else MAIN_ROOT / '.build/jdk27'
        release = jdk / 'release'
        if release.exists():
            fields = dict(line.split('=', 1) for line in release.read_text().splitlines() if '=' in line)
            version = fields.get('JAVA_RUNTIME_VERSION') or fields.get('JAVA_VERSION')
            if not version:
                raise ValueError(f'JDK release file {release} has no JAVA_RUNTIME_VERSION or JAVA_VERSION')
            return version.strip('"')
        return subprocess.check_output([str(jdk / 'bin/java'), *JVM_PROBE_FLAGS, '-version'], text=True,
                                       stderr=subprocess.STDOUT).strip()
    raise ValueError('unknown system: ' + system)


def recipe(system, impl_dir, bindings_dir, lib_dir, out_dir, variant='plain',
           include_dir=None, vendor_dir=None, jdk=None):
    """W-D2 supplies RUST_LOCK and vendor_rust.py --dest ROOT, which writes
    ROOT/vendor and ROOT/.cargo/config.toml. The stub manifest and its lock
    must match this generator's package/bin identity and dependencies. Builds
    require the helper's config and preserve its bytes unchanged.
    """
    impl, bindings, lib, out = map(lambda p: Path(p).resolve(),
                                  (impl_dir, bindings_dir, lib_dir, out_dir))
    include = Path(include_dir).resolve() if include_dir else lib / 'include'
    vendor = Path(vendor_dir).resolve() if vendor_dir else impl.parent / 'vendor'
    audit = variant == 'audit'
    archive = lib / ('libafcore-audit.a' if audit else 'libafcore.a')
    binary = out / 'benchmark'
    env = {}
    manifest = None
    if system == 'c':
        commands = [[CC, *CFLAGS, *(AUDIT_DEFINES if audit else []),
                     '-I' + str(include), *map(str, sorted(impl.glob('*.c'))),
                     str(archive), '-o', str(binary)]]
        launch = [str(binary)]
    elif system == 'tbb':
        commands = [[CXX, *CPP_FLAGS, *(AUDIT_DEFINES if audit else []),
                     '-I' + str(bindings / 'cpp'), '-I' + str(include),
                     *map(str, sorted(impl.glob('*.cpp'))), str(archive), '-ltbb', '-pthread',
                     '-o', str(binary)]]
        launch = [str(binary)]
    elif system == 'antfarm':
        commands = [['ldc2', *D_FLAGS, *(['-d-version=AfAudit'] if audit else []),
                     '-I' + str(bindings / 'd'), '-I' + str(vendor / 'antfarm'),
                     '-I' + str(vendor / 'antfarm/threadpool/source'),
                     *map(str, sorted(impl.glob('*.d'))), str(archive), '-of=' + str(binary)]]
        launch = [str(binary)]
    elif system == 'rayon':
        dependency = os.path.relpath(bindings / 'rust/afcore-sys', impl.parent)
        manifest = ('[package]\nname = ' + json.dumps(RUST_PACKAGE) + '\nversion = "0.1.0"\nedition = "2024"\n'
                    '[[bin]]\nname = ' + json.dumps(RUST_BINARY) + '\npath = "impl/main.rs"\n'
                    '[dependencies]\nafcore-sys = { path = ' + json.dumps(dependency) + ' }\n'
                    'rayon = "=' + RAYON_VERSION + '"\n[features]\naudit = ["afcore-sys/audit"]\n'
                    '[profile.release]\nopt-level = 3\nlto = false\n')
        commands = [['cargo', *RUST_FLAGS, '--manifest-path', str(impl.parent / 'Cargo.toml'),
                     '--target-dir', str(out / 'target'), *(['--features', 'audit'] if audit else [])]]
        env = {'CARGO_HOME': str(impl.parent / '.build/cargo-home'), 'AFCORE_LIB_DIR': str(lib)}
        binary = out / 'target/release' / RUST_BINARY
        launch = [str(binary)]
    elif system == 'fjp':
        jdk = Path(jdk) if jdk else MAIN_ROOT / '.build/jdk27'
        tmpdir = '-Djava.io.tmpdir=' + str(impl.parent / '.build/tmp')
        commands = [[str(jdk / 'bin/javac'), *JAVA_FLAGS,
                     *('-J' + flag for flag in (*JVM_PROBE_FLAGS, tmpdir)), '-d', str(out / 'classes'),
                     *map(str, sorted((bindings / 'java/afbench').rglob('*.java'))),
                     *map(str, sorted(impl.rglob('*.java')))]]
        binary = out / 'classes/GolStub.class'
        launch = [str(jdk / 'bin/java'), *JVM_FLAGS, tmpdir,
                  '-Dafbench.audit=' + str(audit).lower(),
                  '-Dafbench.lib=' + str(lib / ('libafcore-audit.so' if audit else 'libafcore.so')),
                  '-cp', str(out / 'classes'), 'GolStub']
    else:
        raise ValueError('unknown system: ' + system)
    return {'commands': commands, 'launch': launch, 'env': env, 'cwd': str(impl.parent),
            'binary': str(binary), 'manifest': manifest, 'variant': variant}


def build(system, impl_dir, bindings_dir, lib_dir, out_dir, variant='plain', **kwargs):
    version = kwargs.pop('system_version', None)
    plan = recipe(system, impl_dir, bindings_dir, lib_dir, out_dir, variant, **kwargs)
    out = Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if plan['manifest'] is not None:
        workspace = Path(plan['cwd'])
        config = workspace / '.cargo/config.toml'
        if not config.is_file():
            raise ValueError(f'Rust build requires vendor_rust.py --dest {workspace}')
        (workspace / 'Cargo.toml').write_text(plan['manifest'])
        Path(plan['env']['CARGO_HOME']).mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, **plan['env'])
    temporary = Path(plan['cwd']) / '.build/tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    env['TMPDIR'] = str(temporary)
    for command in plan['commands']:
        subprocess.run(command, cwd=plan['cwd'], env=env, check=True)
    compiler = shutil.which('rustc' if system == 'rayon' else plan['commands'][0][0])
    result = {key: value for key, value in plan.items() if key != 'manifest'}
    result.update(system_version=version if version is not None else system_version(system, jdk=kwargs.get('jdk')),
                  binary_sha256=digest(plan['binary']),
                  compiler_executable=compiler,
                  compiler_sha256=digest(compiler),
                  compiler_version=subprocess.check_output(
                      [compiler, *(['-J' + flag for flag in JVM_PROBE_FLAGS] if system == 'fjp' else []),
                       '--version'], text=True, stderr=subprocess.STDOUT).strip())
    (out / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', required=True, choices=['c', 'tbb', 'rayon', 'antfarm', 'fjp'])
    for name in ('impl', 'bindings', 'lib', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--include')
    parser.add_argument('--vendor')
    parser.add_argument('--jdk')
    parser.add_argument('--system-version')
    parser.add_argument('--variant', choices=['plain', 'audit'], default='plain')
    args = parser.parse_args(argv)
    try:
        build(args.system, args.impl, args.bindings, args.lib, args.out, args.variant,
              include_dir=args.include, vendor_dir=args.vendor, jdk=args.jdk,
              system_version=args.system_version)
    except ValueError as error:
        print(f'afbuild.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
