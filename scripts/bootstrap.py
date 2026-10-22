#!/usr/bin/env python3
"""Prepare the untracked inputs the kit needs: upstream/antfarm at the pin and the offline Rayon crates.

Usage:
  python3 -B scripts/bootstrap.py [--antfarm-source URL_OR_PATH] [--cargo-home PATH | --fetch-crates]

- Antfarm: cloned into upstream/antfarm and checked out at the pinned commit (tag afbench-dev-<short>).
  1.9.1-progress is local-only until pushed, so pass the local repository (or set AFBENCH_ANTFARM_MIRROR).
- Rayon: the seven pinned crates are copied from an existing cargo home (default ~/.cargo) into
  .build/cargo-home, or fetched with network access (--fetch-crates).
Both destinations are git-ignored. Re-running checks and keeps what is already correct.
"""
import argparse,os,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts/unified'))
PIN='5f257e5ab894307be6e1e73f4c64fdb23743a085'
CRATES=('rayon-1.10.0','rayon-core-1.12.1','crossbeam-deque-0.8.6','crossbeam-epoch-0.9.18','crossbeam-utils-0.8.21','either-1.13.0','libc-0.2.169')
REGISTRY='index.crates.io-1949cf8c6b5b557f'

def antfarm(source):
    dest=ROOT/'upstream/antfarm'
    if not dest.exists():
        dest.parent.mkdir(exist_ok=True)
        subprocess.run(['git','clone','-q','--no-checkout',source,str(dest)],check=True)
    subprocess.run(['git','fetch','-q',source,PIN],cwd=dest,check=False)
    subprocess.run(['git','checkout','-q',PIN],cwd=dest,check=True)
    subprocess.run(['git','tag','-f',f'afbench-dev-{PIN[:7]}',PIN],cwd=dest,check=True,stdout=subprocess.DEVNULL)
    print('antfarm',subprocess.check_output(['git','log','--oneline','-1'],cwd=dest,text=True).strip())

def crates(cargo_home,fetch):
    reg=ROOT/'.build/cargo-home/registry'
    if fetch:
        stub=ROOT/'benchmarks/unified/bindings/rust/afcore-sys/stub/Cargo.toml'
        subprocess.run(['cargo','fetch','--manifest-path',str(stub)],env=dict(os.environ,CARGO_HOME=str(ROOT/'.build/cargo-home')),check=True)
    else:
        src=Path(cargo_home).expanduser()/'registry'
        for crate in CRATES:
            for sub,name in (('src',crate),('cache',crate+'.crate')):
                a=src/sub/REGISTRY/name;b=reg/sub/REGISTRY/name
                if b.exists():continue
                if not a.exists():raise SystemExit(f'missing {a}; use --fetch-crates or --cargo-home')
                b.parent.mkdir(parents=True,exist_ok=True)
                (shutil.copytree if a.is_dir() else shutil.copy2)(a,b)
    missing=[c for c in CRATES if not (reg/'src'/REGISTRY/c).is_dir() or not (reg/'cache'/REGISTRY/(c+'.crate')).is_file()]
    if missing:raise SystemExit(f'crates still missing: {missing}')
    print('rayon crates ok:',', '.join(CRATES))

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--antfarm-source',default=os.environ.get('AFBENCH_ANTFARM_MIRROR','https://github.com/nriddick/antfarm.git'))
    g=p.add_mutually_exclusive_group();g.add_argument('--cargo-home',default='~/.cargo');g.add_argument('--fetch-crates',action='store_true')
    a=p.parse_args()
    antfarm(a.antfarm_source);crates(a.cargo_home,a.fetch_crates)
    import mkbundle
    for system in ('antfarm','rayon','tbb'):
        assert mkbundle.main(['--system',system,'--check'])==0,f'{system} documentation bundle does not match the manifest'
    print('bootstrap complete')

if __name__=='__main__':main()
