"""Paired frozen-source evaluation of producer stages (parent-trusted runners and core).

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/evaluate.py MODEL SYSTEM NAME \
          --labels base=COMMIT cand=COMMIT [--placement phys|split-smt] [--workers 8 15] [--reps 2]

Each label's commit is archived into a fresh tree; the parent's runners, payload and
prebuilt core replace whatever the commit carries, so only producer-editable files differ.
Binaries are built once per label. Under one exclusive host lock, every configuration first
gets one audit process per label (correctness gate), then REPS rounds in which every label
runs one fresh plain process in shuffled order. Native: run(generations) wall time at fixed
per-board cell updates. Frames: median complete-frame wall time per process.
Ratios are label/first-label per round; geomeans are reported per workload.
"""
import argparse,json,math,random,shutil,statistics,subprocess,sys,tarfile,time
from pathlib import Path
import agents as ag
ROOT=ag.ROOT
sys.path.insert(0,str(ROOT/'scripts/unified'));import microframe as mf

NATIVE_UPDATES={64:1<<28,256:1<<30,1024:1<<32}
RECIPES={'frame-gol-small':(1000,50),'frame-gol-heavy':(100,10),'gol-c-64':(2000,20),'gol-c-256':(500,20),'gol-c-1024':(64,20)}
TRUSTED=['scripts/unified/afbuild.py','scripts/unified/topology.py','scripts/unified/microframe.py','scripts/unified/gol_frame.py']

def freeze(model,system,label,commit,dest):
    repo=ROOT/'.build/open-frame'/ag.WAVE/'eval'/f'{model}-{system}'/f'{dest.name}-{label}'
    repo.mkdir(parents=True)
    commit,_,repo_path=commit.partition('@')
    archive=dest/f'{label}.tar';subprocess.run(['git','archive','-o',str(archive),commit],cwd=repo_path or ag.workspace(model,system),check=True)
    with tarfile.open(archive) as t:t.extractall(repo,filter='data')
    archive.unlink()
    for rel in TRUSTED:shutil.copy2(ROOT/rel,repo/rel)
    shutil.rmtree(repo/'benchmarks/unified/microframe/payload',ignore_errors=True);shutil.copytree(ag.PAYLOAD,repo/'benchmarks/unified/microframe/payload',ignore=shutil.ignore_patterns('core'))
    shutil.rmtree(repo/'benchmarks/unified/core/prebuilt',ignore_errors=True);shutil.copytree(ag.PAYLOAD/'core',repo/'benchmarks/unified/core/prebuilt')
    code=("import sys;sys.path.insert(0,'scripts/unified');from pathlib import Path;import gol_frame as g;import microframe as m;"
          "g.native_build(Path(sys.argv[1]),sys.argv[3]);m.build(Path(sys.argv[2]),systems=(sys.argv[3],))")
    native=dest/f'build-{label}-native';frame=dest/f'build-{label}-frame';native.mkdir();frame.mkdir()
    with (dest/f'build-{label}.log').open('w') as log:
        subprocess.run([sys.executable,'-B','-c',code,str(native),str(frame),system],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
    return {'commit':commit,'repo':str(repo),'native':json.loads((native/'build.json').read_text()),'frame':json.loads((frame/'build.json').read_text())}

def native_argv(build,variant,w,cpus,control,size,generations,out,name):
    b=build['native'][variant];knobs=out/f'{name}-knobs.json';ag.write(knobs,{'grain':0})
    params={'w':size,'h':size,'generations':generations,'density_pct':35}
    return [*b['launch'],'--pass','audit' if variant=='audit' else 'cold','--workers',str(w),'--cpus',','.join(map(str,cpus)),'--control-cpu',str(control),
            '--cpu-slots','--open-hosting','--wait','native','--params',json.dumps(params),'--seed','24301','--knobs',str(knobs),
            '--out',str(out/f'{name}.jsonl'),'--ref-cache',str(ROOT/'.build/open-frame/ref-cache'),'--min-warmup-ms','200','--warmup-cap-ms','2000',
            *(['--audit-required'] if variant=='audit' else [])]

def frame_argv(build,variant,w,cpus,control,recipe_txt,frames,warmup):
    binary=next(v for v in build['frame']['variants'] if v['variant']==variant)['binaries'][build['system']]['path']
    return [binary,str(frames),str(warmup),str(w),','.join(map(str,cpus)),str(control),'24301',str(recipe_txt),'open']

def run_one(argv,out,name,timeout,kind,frames=None):
    r={'name':name,'argv':argv};start=time.monotonic()
    with (out/f'{name}.stdout').open('w') as so,(out/f'{name}.stderr').open('w') as se:
        try:r['exit_code']=subprocess.run(argv,stdout=so,stderr=se,timeout=timeout).returncode
        except subprocess.TimeoutExpired:r['exit_code']=124
    r['process_wall_s']=time.monotonic()-start
    if kind=='native':
        p=out/f'{name}.jsonl';rows=[json.loads(l) for l in p.read_text().splitlines()] if p.exists() else []
        ok=r['exit_code']==0 and len(rows)==1 and rows[0]['status']=='ok' and rows[0]['pin_ok'] is True
        if rows:
            x=rows[0];r['value_ns']=x['timing']['wall_ns'] if x.get('timing') else None
            r['participation']=x.get('participation');r['caller_slot']=(x.get('placement') or {}).get('caller_slot');r['error']=x.get('error')
    else:
        rows=[];final=None
        for l in (out/f'{name}.stdout').read_text().splitlines():
            try:x=json.loads(l)
            except ValueError:continue
            if x.get('schema')=='afbench-microframe-placement/1':final=x
            else:rows.append(x)
        ok=(r['exit_code']==0 and final is not None and final['status']=='ok' and len(rows)==frames
            and all(x['status']=='ok' and x['checksum_ok'] and x['pin_ok'] and x['hosting']=='open' for x in rows))
        measured=[x['wall_ns'] for x in rows if not x['warmup']]
        if measured:r['value_ns']=statistics.median(measured);r['window_ns']=sum(measured)
        if final:r['slot_items']=final['slot_items'];r['caller_slot']=(final['placement'] or {}).get('caller_slot')
    r['passed']=bool(ok);return r

KEEP=('summary.json','processes.json','metadata.json','BUILDS.json','SELECTION.json','DISPOSITION.json','executed-evaluate.py')
def archive_raw(dest):
    """Move per-process records, knobs, recipes and build logs into DEST/raw.tar.gz."""
    import tarfile
    files=[p for p in sorted(dest.rglob('*')) if p.is_file() and p.name not in KEEP and p.name!='raw.tar.gz']
    with tarfile.open(dest/'raw.tar.gz','w:gz') as t:
        for p in files:t.add(p,arcname=str(p.relative_to(dest)))
    for p in files:p.unlink()
    for d in sorted((d for d in dest.rglob('*') if d.is_dir()),key=lambda d:-len(d.parts)):
        if not any(d.iterdir()):d.rmdir()

def descriptive(kind,what):return kind=='frame' and str(what).startswith('gol-c-')

def geomean(xs):return math.exp(sum(map(math.log,xs))/len(xs)) if xs else None

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('model');p.add_argument('system');p.add_argument('name')
    p.add_argument('--labels',nargs='+',required=True,help='label=COMMIT or label=COMMIT@REPO (another workspace); the first label is the ratio base')
    p.add_argument('--placement',default='phys',choices=['phys','split-smt'])
    p.add_argument('--workers',type=int,nargs='+',default=[8,15]);p.add_argument('--reps',type=int,default=2)
    p.add_argument('--descriptive-reps',type=int,default=1,help='pairs for standalone opaque-C recipes, which are descriptive only (user decision 2026-10-21)')
    p.add_argument('--sizes',type=int,nargs='*',default=[64,256,1024]);p.add_argument('--recipes',nargs='*',default=list(RECIPES))
    a=p.parse_args()
    with ag.quiet(True):run(a)

def run(a):
    dest=ag.OUT/f'{a.model}-{a.system}'/a.name;dest.mkdir(parents=True)
    shutil.copy2(__file__,dest/'executed-evaluate.py')
    labels=[x.split('=',1) for x in a.labels]
    profile=mf.topology.discover();control=profile['policies']['phys']['control_cpu']
    builds={}
    for label,commit in labels:builds[label]={**freeze(a.model,a.system,label,commit,dest),'system':a.system}
    ag.write(dest/'BUILDS.json',builds)
    for recipe in a.recipes:
        spec=json.loads((ROOT/f'benchmarks/unified/microframe/{recipe}.json').read_text())
        (dest/f'{recipe}.txt').write_text(''.join(f"{b['kind']} {b['items']} {b['iterations']} {b['grain']} {b['waves']}\n" for b in spec['batches'] for _ in range(b['repeat'])))
    configs=[('native',s,w) for w in a.workers for s in a.sizes]+[('frame',r,w) for w in a.workers for r in a.recipes]
    rng=random.Random(24301);records=[]
    meta={'labels':dict(labels),'placement':a.placement,'workers':a.workers,'reps':a.reps,'native_updates':{str(k):v for k,v in NATIVE_UPDATES.items()},
          'recipes':{r:RECIPES[r] for r in a.recipes},'slot_maps':{w:mf.placement(profile,a.placement,w) for w in a.workers},'control_cpu':control,
          'timing':'native: C run(generations) wall after >=200 ms warmup; frame: median of measured complete-frame wall per process; process wall separate',
          'started':time.time()}
    ag.write(dest/'metadata.json',meta)
    (ROOT/'.build/open-frame/ref-cache').mkdir(parents=True,exist_ok=True)
    mf.DEFAULT_LOCK.parent.mkdir(parents=True,exist_ok=True);mf.DEFAULT_LOCK.touch(exist_ok=True)
    with mf.host_lock(mf.DEFAULT_LOCK,exclusive=True):
        plan=[]
        for kind,what,w in configs:
            for label,_ in labels:plan.append((kind,what,w,label,'audit',-1))
        for rep in range(a.reps):
            for kind,what,w in configs:
                if descriptive(kind,what) and rep>=a.descriptive_reps:continue
                order=[l for l,_ in labels];rng.shuffle(order)
                plan+=[(kind,what,w,label,'plain',rep) for label in order]
        for i,(kind,what,w,label,variant,rep) in enumerate(plan):
            cpus=mf.placement(profile,a.placement,w);name=f'{i:04}-{label}-{kind}-{what}-w{w}-{variant}' + (f'-r{rep}' if rep>=0 else '')
            if kind=='native':
                gens=16 if variant=='audit' else NATIVE_UPDATES[what]//(what*what)
                r=run_one(native_argv(builds[label],variant,w,cpus,control,what,gens,dest,name),dest,name,600,'native')
            else:
                frames,warmup=RECIPES[what] if variant=='plain' else (3,2)
                r=run_one(frame_argv(builds[label],variant,w,cpus,control,dest/f'{what}.txt',frames,warmup),dest,name,900,'frame',frames+warmup)
            r.update(kind=kind,what=what,workers=w,label=label,variant=variant,rep=rep);records.append(r)
            ag.write(dest/'processes.json',records);print(name,'PASS' if r['passed'] else 'FAIL',flush=True)
    base=labels[0][0];summary={'labels':dict(labels),'placement':a.placement,'configs':[],'failures':[r['name'] for r in records if not r['passed']]}
    for kind,what,w in configs:
        row={'kind':kind,'what':what,'workers':w}
        for label,_ in labels:
            rs=[r for r in records if (r['kind'],r['what'],r['workers'],r['label'])==(kind,what,w,label)]
            row[label]={'audit_ok':all(r['passed'] for r in rs if r['variant']=='audit'),'plain_ok':all(r['passed'] for r in rs if r['variant']=='plain'),
                        'values_ns':[r.get('value_ns') for r in rs if r['variant']=='plain']}
        for label,_ in labels[1:]:
            ratios=[]
            for rep in range(a.reps):
                pair=[next((r for r in records if (r['kind'],r['what'],r['workers'],r['label'],r['variant'],r['rep'])==(kind,what,w,l,'plain',rep)),None) for l in (base,label)]
                if all(x and x['passed'] and x.get('value_ns') for x in pair):ratios.append(pair[1]['value_ns']/pair[0]['value_ns'])
            row[f'{label}/{base}']=ratios
        summary['configs'].append(row)
    for label,_ in labels[1:]:
        key=f'{label}/{base}'
        # The composite frames alone judge the frame adapter; opaque-C is descriptive.
        for name,match in (('native',lambda c:c['kind']=='native'),('frame',lambda c:c['kind']=='frame' and not descriptive('frame',c['what'])),
                           ('opaque_c_descriptive',lambda c:descriptive(c['kind'],c['what']))):
            summary[f'geomean_{name}_{key}']=geomean([x for c in summary['configs'] if match(c) for x in c[key]])
    ag.write(dest/'summary.json',summary)
    archive_raw(dest)
    print(json.dumps({k:v for k,v in summary.items() if k.startswith('geomean') or k=='failures'},indent=1))

if __name__=='__main__':main()
