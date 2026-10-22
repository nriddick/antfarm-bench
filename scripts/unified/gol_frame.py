#!/usr/bin/env python3
"""Small evidence runner for native GoL and opaque C frame/GoL phases."""
import argparse,json,random,subprocess,time,sys,resource
from pathlib import Path
import microframe as mf
import afbuild
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'benchmarks/unified/core'))
import build_core

def write(p,x):p.write_text(json.dumps(x,indent=2)+'\n')
def native_build(out,system):
    lib=ROOT/'.build/gol-frame/core';build_core.build(lib)
    records={}
    for v in ('plain','audit'):
        records[v]=afbuild.build(system,ROOT/'impl',ROOT/'benchmarks/unified/bindings',lib,ROOT/'.build/gol-frame'/v,v,include_dir=ROOT/'benchmarks/unified/core/include',vendor_dir=ROOT/'vendor')
    if system=='fjp':
        for v,r in records.items():
            r['closure']={str(p):mf.sha(p) for p in [lib/('libafcore-audit.so' if v=='audit' else 'libafcore.so'),*sorted((Path(r['binary']).parent).rglob('*.class'))]}
    write(out/'build.json',records);return records

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--system',default='antfarm',choices=['antfarm','tbb','rayon','fjp'])
    p.add_argument('--workers',nargs='+',type=int,default=[8,15]);p.add_argument('--placement',default='phys',choices=['phys','split-smt'])
    p.add_argument('--hosting',default='open',choices=['open','pinned'],help='open: allocation-contained affinity, caller may be one of the W registered workers, waiting is the implementation\'s choice')
    p.add_argument('--sizes',nargs='+',type=int,default=[64,256,1024]);p.add_argument('--updates',type=int,default=16777216)
    p.add_argument('--passes',nargs='+',default=['check','audit','cold'],choices=['check','audit','verify','cold','trial'])
    p.add_argument('--reps',type=int,default=1);p.add_argument('--grains',type=int,nargs='+',default=[0]);p.add_argument('--timeout',type=int,default=120)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    profile=mf.topology.discover();control=profile['policies']['phys']['control_cpu']
    maps={w:mf.placement(profile,a.placement,w) for w in a.workers}
    write(out/'metadata.json',{'parameters':{k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()},'topology':profile,'worker_maps':maps,'control_cpu':control,'hosting':a.hosting,'timing':'native C driver run(generations) only; actual timing.wall_ns, process wall separately; --updates specifies fixed work, not a guaranteed window'})
    plan=[dict(workers=w,size=s,grain=g,rep=r,pass_=ps) for r in range(a.reps) for w in a.workers for g in a.grains for ps in a.passes for s in ([64] if ps=='check' else ([64,256] if ps=='audit' else a.sizes))]
    random.Random(24301).shuffle(plan);write(out/'plan.json',plan);results=[]
    mf.DEFAULT_LOCK.parent.mkdir(parents=True,exist_ok=True);mf.DEFAULT_LOCK.touch(exist_ok=True)
    with mf.host_lock(mf.DEFAULT_LOCK,exclusive=True):
        builds=native_build(out,a.system)
        for i,t in enumerate(plan):
            w=t['workers'];size=t['size'];ps=t['pass_'];variant='audit' if ps=='audit' else 'plain';b=builds[variant]
            name=f'{i:03}-{ps}-{size}-w{w}-g{t["grain"]}-r{t["rep"]}'
            knobs=out/(name+'-knobs.json');write(knobs,{'grain':t['grain']})
            params={'w':size,'h':size,'generations':(16 if ps=='audit' else max(1,a.updates//(size*size))),'density_pct':35}
            argv=[*b['launch'],'--pass',ps,'--workers',str(w),'--cpus',','.join(map(str,maps[w])),'--control-cpu',str(control),'--cpu-slots',*(['--open-hosting','--wait','native'] if a.hosting=='open' else ['--wait','spin' if a.system=='antfarm' else 'native']),'--params',json.dumps(params),'--seed','24301','--knobs',str(knobs),'--out',str(out/(name+'.jsonl')),'--ref-cache',str(ROOT/'.build/gol-frame/ref-cache'),'--min-warmup-ms','200','--warmup-cap-ms','2000']
            if ps=='audit':argv+=['--audit-required']
            assert mf.sha(b['binary'])==b['binary_sha256']
            assert all(mf.sha(p)==h for p,h in b.get('closure',{}).items())
            r={**t,'name':name,'argv':argv,'variant':variant,'binary_sha256':b['binary_sha256']};start=time.monotonic();u=resource.getrusage(resource.RUSAGE_CHILDREN)
            with (out/(name+'.stdout')).open('w') as stdout,(out/(name+'.stderr')).open('w') as stderr:
                try:r['exit_code']=subprocess.run(argv,cwd=ROOT,stdout=stdout,stderr=stderr,timeout=a.timeout).returncode
                except subprocess.TimeoutExpired:r['exit_code']=124
            v=resource.getrusage(resource.RUSAGE_CHILDREN);r.update(process_wall_s=time.monotonic()-start,process_cpu_user_s=v.ru_utime-u.ru_utime,process_cpu_sys_s=v.ru_stime-u.ru_stime)
            path=out/(name+'.jsonl');rows=[]
            try:rows=[json.loads(l) for l in path.read_text().splitlines()] if path.exists() else []
            except ValueError as e:r['parse_error']=str(e)
            r['records']=len(rows);r['passed']=r['exit_code']==0 and len(rows)==1 and rows[0]['status']=='ok' and rows[0]['pin_ok'] is True
            if rows:
                r['result']=rows[0]
                cells=(rows[0].get('participation') or {}).get('slot_cells')
                if cells is not None:r['participating_slots']=sum(1 for x in cells if x)
            results.append(r);write(out/'processes.json',results);print(name,'PASS' if r['passed'] else 'FAIL',flush=True)
    write(out/'summary.json',{'processes':len(results),'passed':sum(r['passed'] for r in results),'failed':[r['name'] for r in results if not r['passed']]})
    return int(any(not r['passed'] for r in results))
if __name__=='__main__':sys.exit(main())
