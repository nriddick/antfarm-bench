#!/usr/bin/env python3
"""Run a composite frame of scheduler microbenchmark batches. Feasibility pilot.
Example: python3 -B scripts/unified/microframe.py --out results/unified/microframe/DATE
"""
import argparse
from contextlib import contextmanager
import fcntl
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import time
import shlex
import shutil

import afbuild
import topology
DEFAULT_LOCK=afbuild.MAIN_ROOT/'.build/unified/host.lock'

@contextmanager
def host_lock(path,exclusive=False):
    with Path(path).open('rb') as stream:
        fcntl.flock(stream,fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        try:yield
        finally:fcntl.flock(stream,fcntl.LOCK_UN)

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'benchmarks/unified/core'))
import build_core
SOURCE=ROOT/'benchmarks/unified/microframe'
BUILD=ROOT/'.build/unified/microframe'
# Centrally compiled shared payload/driver objects, one per variant. When
# present they are linked unchanged into every adapter instead of compiling
# kernels.c locally; manifest.json records their hashes and build.
PAYLOAD=SOURCE/'payload'

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def write(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def command(argv,log):
    r=subprocess.run(list(map(str,argv)),cwd=ROOT,text=True,capture_output=True)
    log.write_text(r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(f'command failed ({r.returncode}): {log}')
    return r

def build(out,systems=('serial','tbb')):
    BUILD.mkdir(parents=True,exist_ok=True)
    core=build_core.build(BUILD/'core',variants=('plain',))['plain']['a']
    records=[]
    includes=['-I'+str(SOURCE),'-I'+str(ROOT/'benchmarks/unified/core/include'),
              '-I'+str(ROOT/'benchmarks/unified/core/src')]
    payload=json.loads((PAYLOAD/'manifest.json').read_text()) if (PAYLOAD/'manifest.json').exists() else None
    for variant in ('plain','audit'):
        if payload:
            obj=PAYLOAD/f'kernels-{variant}.o'
            if sha(obj)!=payload['objects'][f'kernels-{variant}.o']:raise RuntimeError(f'payload object {obj} does not match its manifest')
            commands=[]
        else:
            obj=BUILD/f'kernels-{variant}.o'
            commands=[[afbuild.CC,*afbuild.CFLAGS,*includes,
                *(['-DMF_AUDIT'] if variant=='audit' else []),'-c',SOURCE/'kernels.c','-o',obj]]
        for system in systems:
            binary=BUILD/f'{system}-{variant}'
            if system=='serial':
                args=[afbuild.CC,*afbuild.CFLAGS,*includes,SOURCE/'serial.c',obj,core,'-o',binary]
            elif system=='tbb':
                args=[afbuild.CXX,*afbuild.CPP_FLAGS,'-pthread',*includes,
                      '-I'+str(ROOT/'benchmarks/unified/bindings/cpp'),SOURCE/'tbb.cpp',obj,core,'-ltbb','-o',binary]
            elif system=='rayon':
                project=ROOT/'frame';project.mkdir(exist_ok=True)
                (project/'Cargo.toml').write_text('[package]\nname="afbench-frame"\nversion="0.1.0"\nedition="2024"\n[[bin]]\nname="frame"\npath="../benchmarks/unified/microframe/frame_rust.rs"\n[dependencies]\nafcore-sys={path="../benchmarks/unified/bindings/rust/afcore-sys"}\nrayon="=1.10.0"\n')
                # Generate an offline lock once from the pinned vendored dependency tree.
                env=dict(os.environ,AFCORE_LIB_DIR=str(BUILD/'core'),CARGO_HOME=str(ROOT/'.build/cargo-home'),
                         RUSTFLAGS='-C link-arg='+str(obj)+' -C link-arg='+str(core))
                if commands:command(commands[0],out/f'build-{variant}-kernel.log')
                lockcmd=['cargo','generate-lockfile','--offline','--manifest-path',str(project/'Cargo.toml')]
                for cmd in (lockcmd,['cargo',*afbuild.RUST_FLAGS,'--manifest-path',str(project/'Cargo.toml'),'--target-dir',str(BUILD/f'rust-{variant}')]):
                    r=subprocess.run(cmd,cwd=ROOT,env=env,text=True,capture_output=True)
                    with (out/f'build-{variant}-rust.log').open('a') as log:log.write(r.stdout+r.stderr)
                    if r.returncode:raise RuntimeError('Rust frame build failed: '+str(out))
                shutil.copy2(BUILD/f'rust-{variant}/release/frame',binary)
                commands += [lockcmd,cmd]
                continue
            elif system=='fjp':
                library=BUILD/f'libframe-{variant}.so';classes=BUILD/f'classes-{variant}';classes.mkdir(exist_ok=True)
                commands += [[afbuild.CC,*afbuild.CFLAGS,*includes,'-shared',SOURCE/'frame_ffi.c',obj,'-Wl,--whole-archive',core,'-Wl,--no-whole-archive','-o',library]]
                jdk=afbuild.MAIN_ROOT/'.build/jdk27'
                sources=[*sorted((ROOT/'benchmarks/unified/bindings/java').rglob('*.java')),SOURCE/'FrameMain.java',SOURCE/'FrameImpl.java']
                args=[jdk/'bin/javac','-J-XX:-UsePerfData',*afbuild.JAVA_FLAGS,'-d',classes,*sources]
                launch=[str(jdk/'bin/java'),*afbuild.JVM_FLAGS,'-Dafbench.lib='+str(library),'-cp',str(classes),'FrameMain']
                binary.write_text('#!/bin/sh\nexec '+shlex.join(launch)+' "$@"\n');binary.chmod(0o755)
            else:
                vendor=ROOT/'vendor/antfarm'
                args=['ldc2',*afbuild.D_FLAGS,
                      '-I'+str(SOURCE),'-I'+str(ROOT/'benchmarks/unified/bindings/d'),
                      '-I'+str(vendor),'-I'+str(vendor/'threadpool/source'),
                      SOURCE/'entry_antfarm.d',SOURCE/'antfarm_frame.d',obj,core,
                      '-of='+str(binary)]
            commands.append(args)
        for i,args in enumerate(commands):
            if str(args[0])!='cargo':command(args,out/f'build-{variant}-{i}.log')
        records.append({'variant':variant,'payload_object':{'path':str(obj),'sha256':sha(obj),'prebuilt':bool(payload)},'commands':[list(map(str,c)) for c in commands],
                        'binaries':{s:{'path':str(BUILD/f'{s}-{variant}'),'sha256':sha(BUILD/f'{s}-{variant}'), 'closure':({str(p):sha(p) for p in [BUILD/f'libframe-{variant}.so',*sorted((BUILD/f'classes-{variant}').rglob('*.class'))]} if s=='fjp' else {})} for s in systems}})
    inputs=list(SOURCE.rglob('*'))+list((ROOT/'benchmarks/unified/core').rglob('*.c'))+list((ROOT/'benchmarks/unified/core').rglob('*.h'))
    inputs += [Path(__file__),ROOT/'scripts/unified/afbuild.py',ROOT/'scripts/unified/topology.py',ROOT/'benchmarks/unified/core/build_core.py',ROOT/'benchmarks/unified/bindings/cpp/pool_tbb.hpp',ROOT/'benchmarks/unified/bindings/cpp/afcore.hpp']
    inputs += list((ROOT/'impl').rglob('*'))
    for lang in ('rust','java'):
        inputs += list((ROOT/'benchmarks/unified/bindings'/lang).rglob('*'))
    if 'antfarm' in systems:
        inputs += list((ROOT/'vendor/antfarm').rglob('*'))
        inputs += list((ROOT/'benchmarks/unified/bindings/d').glob('*.d'))
        inputs += [ROOT/'ANTFARM-PIN.json']
    snapshot=out/'source';snapshot.mkdir()
    shutil.copytree(SOURCE,snapshot/'microframe')
    shutil.copy2(__file__,snapshot/'microframe.py')
    write(out/'build.json',{'variants':records,'sources':{str(p.relative_to(ROOT)):sha(p) for p in inputs if p.is_file()},
        'core_archive_sha256':sha(core),'compilers':{c:subprocess.check_output([c,'--version'],text=True) for c in dict.fromkeys([afbuild.CC,*([afbuild.CXX] if 'tbb' in systems else []),*(['ldc2'] if 'antfarm' in systems else [])])},
        **({'tbb':afbuild.system_version('tbb')} if 'tbb' in systems else {}),
        **({'payload':payload} if payload else {}),
        **({'antfarm':json.loads((ROOT/'ANTFARM-PIN.json').read_text())} if 'antfarm' in systems else {})})
    return records

def row_ok(row,trial):
    audited_worker=trial['variant']=='audit' and trial['system']!='serial'
    return (row.get('schema')=='afbench-microframe/3' and row['status']=='ok' and row['checksum_ok'] and row['pin_ok']
            and row['system']==trial['system'] and row['audit']==(trial['variant']=='audit')
            and row['hosting']==('serial' if trial['system']=='serial' else trial['hosting'])
            and (row['coverage_ok'] is True if audited_worker else row['coverage_ok'] is None))

def percentile(values,p):
    # Nearest-rank: small pilots cannot estimate publication tail percentiles.
    import math
    return sorted(values)[max(0,math.ceil(len(values)*p)-1)]

def placement(profile,name,workers):
    base=profile['policies']['phys']
    if name=='phys':
        if not 1<=workers<=len(base['worker_cpus']):raise ValueError('worker count outside phys policy')
        return base['worker_cpus'][:workers]
    if not 1<=workers<=40:raise ValueError('split-smt pilot supports 1..40 workers')
    cores=[c for c in profile['cores'] if base['control_cpu'] not in c['siblings']]
    domains=sorted({c['l3_domain'] for c in cores})
    groups=[[c for c in cores if c['l3_domain']==d] for d in domains]
    ordered=[g[i] for i in range(max(map(len,groups))) for g in groups if i<len(g)]
    cpus=[c['primary'] for c in ordered]
    cpus += [cpu for c in ordered for cpu in c['siblings'] if cpu!=c['primary']]
    if not cpus:raise ValueError('no worker CPUs after reserving control core')
    return [cpus[i%len(cpus)] for i in range(workers)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--reuse-build',type=Path,help='Reuse a prior evidence directory only if every recorded source and binary hash still matches')
    parser.add_argument('--systems',choices=['serial','tbb','antfarm','rayon','fjp'],nargs='+',default=['serial','tbb'])
    parser.add_argument('--variants',choices=['plain','audit'],nargs='+',default=['plain','audit'])
    parser.add_argument('--placement',choices=['phys','split-smt'],default='phys')
    parser.add_argument('--hosting',choices=['open','pinned'],default='open',help='open: workers may use any affinity within the allocation (the distinct slot CPUs) and the caller may register as one of the W; pinned: historical singleton slot pins, controller excluded')
    parser.add_argument('--workers',type=int,nargs='+',default=[1,8,15])
    parser.add_argument('--recipe',type=Path,default=SOURCE/'frame-small.json')
    parser.add_argument('--reps',type=int,default=2)
    parser.add_argument('--frames',type=int,default=30)
    parser.add_argument('--warmup',type=int,default=5)
    parser.add_argument('--seed',type=int,default=24301)
    parser.add_argument('--timeout',type=int,default=30)
    args=parser.parse_args()
    profile=topology.discover();policy=profile['policies']['phys']
    try:worker_maps={w:placement(profile,args.placement,w) for w in args.workers}
    except ValueError as error:parser.error(str(error))
    if min(args.reps,args.frames,args.warmup,args.timeout)<=0:parser.error('positive counts required')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    recipe=json.loads(args.recipe.read_text())
    expanded=[]
    for batch in recipe['batches']:
        if batch['kind'] not in ('m1','m4','m6','gol'):parser.error('supported batch kinds: m1, m4, m6, gol')
        if any(type(batch[k]) is not int or batch[k]<1 for k in ('items','iterations','grain','waves','repeat')):parser.error('batch counts must be positive integers')
        if batch['repeat']>64 or len(expanded)+batch['repeat']>64:parser.error('at most 64 expanded batches')
        expanded.extend([dict(batch)]*batch['repeat'])
    if not expanded:parser.error('empty recipe')
    write(out/'recipe.json',recipe)
    (out/'recipe.txt').write_text(''.join(f"{b['kind']} {b['items']} {b['iterations']} {b['grain']} {b['waves']}\n" for b in expanded))
    start=time.monotonic()
    write(out/'metadata.json',{'classification':'feasibility only; fixed warmup; checksum reads between frames; serial replay after actual sequence; audit timings not performance',
        'hosting':args.hosting,'allocation':'distinct CPUs of each worker slot map; control CPU outside it' if args.hosting=='open' else 'singleton slot pins',
        'started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'argv':sys.argv,
        'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'source_status':subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),
        'host':platform.uname()._asdict(),'placement':args.placement,'worker_slot_maps':worker_maps,'topology':profile,'parameters':{k:str(v) if isinstance(v,Path) else v for k,v in vars(args).items()},
        'timing':'C CLOCK_MONOTONIC around the adapter frame() call, which must execute every batch/wave and finish all work before returning; pool startup/reference/checksum/JSON outside the frame window; audit-only batch spans (first payload start to last payload end, possibly overlapping) are diagnostics',
        'warmup':'fixed count, not convergence; no sleeps or pacing','process_model':f'{args.reps} fresh processes/configuration; randomized interleaving'})
    plan=[{'variant':v,'system':s,'workers':w,'rep':rep,'hosting':args.hosting} for rep in range(args.reps)
          for v in args.variants for s in args.systems for w in ([1] if s=='serial' else args.workers)]
    random.Random(args.seed).shuffle(plan);write(out/'plan.json',plan)
    records=[];failures=[];hashes={}
    DEFAULT_LOCK.parent.mkdir(parents=True,exist_ok=True);DEFAULT_LOCK.touch(exist_ok=True)
    with host_lock(DEFAULT_LOCK,exclusive=True):
        if args.reuse_build:
            prior=args.reuse_build.resolve();record=json.loads((prior/'build.json').read_text())
            if not all((ROOT/p).is_file() and sha(ROOT/p)==h for p,h in record['sources'].items()):
                raise ValueError('reuse-build source identity changed; perform a fresh build')
            builds=record['variants']
            for build_record in builds:
                for system in args.systems:
                    identity=build_record['binaries'][system]
                    if sha(identity['path'])!=identity['sha256'] or not all(sha(p)==h for p,h in identity.get('closure',{}).items()):
                        raise ValueError('reuse-build binary identity changed; perform a fresh build')
            write(out/'build.json',record)
            write(out/'build-reuse.json',{'directory':str(prior),'manifest_sha256':sha(prior/'build.json')})
            shutil.copytree(prior/'source',out/'source')
        else:
            builds=build(out,args.systems)
        for index,trial in enumerate(plan):
            name=f"{index:02}-{trial['system']}-{trial['variant']}-w{trial['workers']}-r{trial['rep']}"
            binary=BUILD/f"{trial['system']}-{trial['variant']}"
            identity=next(v for v in builds if v['variant']==trial['variant'])['binaries'][trial['system']]
            assert sha(binary)==identity['sha256']
            assert all(sha(p)==h for p,h in identity.get('closure',{}).items())
            cpus=placement(profile,args.placement,trial['workers'])
            argv=list(map(str,[binary,args.frames,args.warmup,trial['workers'],
                ','.join(map(str,cpus)),policy['control_cpu'],args.seed,out/'recipe.txt',args.hosting]))
            began=time.monotonic();run={'name':name,**trial,'argv':argv,'binary_sha256':identity['sha256'],'worker_cpus':cpus,'control_cpu':policy['control_cpu']}
            with (out/(name+'.jsonl')).open('w') as stdout,(out/(name+'.stderr')).open('w') as stderr:
                try:
                    proc=subprocess.run(argv,cwd=ROOT,stdout=stdout,stderr=stderr,timeout=args.timeout)
                    run['exit_code']=proc.returncode
                except subprocess.TimeoutExpired:run['exit_code']=124
            run['process_wall_s']=time.monotonic()-began
            rows=[];final=None
            try:
                for l in (out/(name+'.jsonl')).read_text().splitlines():
                    row=json.loads(l)
                    if row.get('schema')=='afbench-microframe-placement/1':final=row
                    else:rows.append(row)
            except ValueError as error:run['parse_error']=str(error)
            if final:
                run['placement']=final['placement'];run['slot_items']=final['slot_items'];run['process_cpu_ns']=final['process_cpu_ns']
                if final['slot_items'] is not None:run['participating_slots']=sum(1 for x in final['slot_items'] if x)
            measured=[r['participants'] for r in rows if not r['warmup'] and r.get('participants') is not None]
            if measured:run['participants_per_frame']={'min':min(measured),'max':max(measured)}
            ok=run['exit_code']==0 and len(rows)==args.frames+args.warmup and final is not None and final['status']=='ok'
            ok&='parse_error' not in run
            for r in rows:
                ok &= row_ok(r,trial)
                hashes.setdefault(r['frame'],set()).add(r['hash'])
            run['records']=len(rows);run['passed']=bool(ok)
            values=[r['wall_ns'] for r in rows if not r['warmup']]
            if values:run['diagnostic_frame_ns']={key:percentile(values,p) for key,p in [('p50',.5),('p99',.99),('p999',.999),('max',1)]}
            records.append(run);write(out/'processes.json',records)
            if not ok:failures.append(name)
            print(name,'PASS' if ok else 'FAIL',flush=True)
    agreement=all(len(h)==1 for h in hashes.values())
    summary={'processes':len(records),'records':sum(r['records'] for r in records),'failed_processes':failures,
             'cross_system_hash_agreement':agreement,'elapsed_s':time.monotonic()-start,
             'qualification':'feasibility only; no product ranking or calibrated cost claims',
             'systems':args.systems,'variants':args.variants,'workload':recipe['name'],'batches_per_frame':len(expanded),'items_per_frame':sum(b['items']*b['waves'] for b in expanded),'recipe_sha256':sha(out/'recipe.json')}
    write(out/'summary.json',summary)
    return 0 if not failures and agreement else 1
if __name__=='__main__':sys.exit(main())
