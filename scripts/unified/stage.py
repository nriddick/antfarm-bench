#!/usr/bin/env python3
"""Stage test commands: one correctness check and one measurement per stage.

  python3 -B scripts/unified/stage.py check   --system S --out evidence/NAME [--placement P] [--workers ...]
  python3 -B scripts/unified/stage.py measure --system S --out evidence/NAME [--placement P] [--workers ...] [--perf]

check   (no timing or profiling): native check/verify passes at 64/256/1024 plus audit builds at 64/256,
        and a short plain and audit run of every frame recipe. Results are compared with the trusted
        references (checksums, final state); audits also check every reported cell and generation,
        the reporting thread, coverage and wave order.
measure (instrumented): native cold timing at 64/256/1024 (2^26/2^28/2^30 cell updates) and plain
        frame timing (composite frames judge the adapter; opaque-C recipes are diagnostic). With
        --perf, also `perf record -g` of native 1024 and frame-gol-heavy at the largest worker count,
        with a per-thread text report of each. Profiles cover the whole process, including untimed
        startup and the serial reference replay on the control thread.
Each command runs the supplied runners, which take the host lock, and prints a compact summary.
"""
import argparse,json,statistics,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/unified'))
import microframe as mf

COMPOSITE=('frame-gol-small','frame-gol-heavy');OPAQUE=('gol-c-64','gol-c-256','gol-c-1024')
UPDATES={64:1<<26,256:1<<28,1024:1<<30}

FAILED_RUNNERS=[]
def run(argv):
    print('+',' '.join(map(str,argv)),flush=True)
    code=subprocess.run([sys.executable,'-B',*map(str,argv)],cwd=ROOT).returncode
    if code:FAILED_RUNNERS.append({'runner':' '.join(map(str,argv[:1]+argv[argv.index('--out'):argv.index('--out')+2])),'exit_code':code})
    return code

def frames(a,out,variant,counts):
    first=None;codes=[]
    for recipe in COMPOSITE+OPAQUE:
        n,w=counts[recipe]
        argv=['scripts/unified/microframe.py','--systems',a.system,'--out',out/f'frame-{recipe}','--recipe',f'benchmarks/unified/microframe/{recipe}.json',
              '--placement',a.placement,'--workers',*a.workers,'--variants',*variant.split(),'--reps','1','--frames',n,'--warmup',w,'--timeout','600']
        if first:argv+=['--reuse-build',first]
        codes.append(run(argv))
        if first is None and (out/f'frame-{recipe}'/'build.json').exists():first=out/f'frame-{recipe}'
    return codes

def summary(out):
    rows=[]
    for p in sorted(out.glob('*/processes.json')):
        for r in json.loads(p.read_text()):
            x={'run':p.parent.name,'name':r['name'],'passed':r['passed']}
            if r.get('diagnostic_frame_ns'):x['frame_p50_us']=round(r['diagnostic_frame_ns']['p50']/1e3,2)
            t=(r.get('result') or {}).get('timing')
            if t:x['native_ms']=round(t['wall_ns']/1e6,3)
            for k in ('participating_slots','participants_per_frame'):
                if k in r:x[k]=r[k]
            rows.append(x)
    (out/'SUMMARY.json').write_text(json.dumps(rows,indent=1)+'\n')
    (out/'RUNNERS.json').write_text(json.dumps({'failed_runners':FAILED_RUNNERS},indent=1)+'\n')
    for x in rows:print(json.dumps(x))
    print('passed',sum(x['passed'] for x in rows),'of',len(rows))
    for f in FAILED_RUNNERS:print('FAILED RUNNER (build or startup; its processes did not all run):',f['runner'],'exit',f['exit_code'])
    return all(x['passed'] for x in rows) and not FAILED_RUNNERS

def perf(a,out):
    """Re-run two recorded processes under perf record (host lock held by flock)."""
    w=max(map(int,a.workers));targets=[]
    if a.workloads!='frames':
        native=json.loads((out/'native-1024'/'processes.json').read_text())
        r=next(r for r in native if r['workers']==w and r['pass_']=='cold')
        argv=list(r['argv']);argv[argv.index('--out')+1]=str(out/'perf-native-1024.jsonl');targets.append(('native-1024',argv))
    if a.workloads!='native':
        frame=json.loads((out/'frame-frame-gol-heavy'/'processes.json').read_text())
        targets.append(('frame-gol-heavy',next(r for r in frame if r['workers']==w)['argv']))
    for name,argv in targets:
        data=out/f'perf-{name}.data'
        subprocess.run(['flock','-x',str(mf.DEFAULT_LOCK),'perf','record','-F','2000','-g','-o',str(data),'--',*argv],cwd=ROOT,
                       stdout=subprocess.DEVNULL,stderr=(out/f'perf-{name}.stderr').open('w'))
        with (out/f'perf-{name}.txt').open('w') as f:
            subprocess.run(['perf','report','-i',str(data),'--stdio','--no-children','--sort','pid,sym','--percent-limit','0.5'],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
        print('perf report:',out/f'perf-{name}.txt',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command',choices=['check','measure']);p.add_argument('--system',required=True,choices=['antfarm','tbb','rayon','fjp'])
    p.add_argument('--out',type=Path,required=True);p.add_argument('--placement',default='phys',choices=['phys','split-smt'])
    p.add_argument('--workers',nargs='+',default=['8','15']);p.add_argument('--perf',action='store_true')
    p.add_argument('--workloads',default='both',choices=['both','native','frames'],help='limit a stage to one workload when its task says so')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    if a.perf and a.command!='measure':p.error('--perf belongs to measure')
    (out/'STAGE-COMMAND.json').write_text(json.dumps({'argv':sys.argv},indent=1)+'\n')
    common=['--system',a.system,'--placement',a.placement,'--workers',*a.workers]
    if a.command=='check':
        if a.workloads!='frames':run(['scripts/unified/gol_frame.py',*common,'--out',out/'native','--passes','check','verify','audit','--sizes','64','256','1024'])
        if a.workloads!='native':frames(a,out,'plain audit',{r:('5','1') for r in COMPOSITE+OPAQUE})
    else:
        if a.workloads!='frames':
            for size,updates in UPDATES.items():
                run(['scripts/unified/gol_frame.py',*common,'--out',out/f'native-{size}','--passes','cold','--sizes',size,'--updates',updates])
        if a.workloads!='native':
            frames(a,out,'plain',{'frame-gol-small':('300','30'),'frame-gol-heavy':('50','10'),'gol-c-64':('500','20'),'gol-c-256':('200','20'),'gol-c-1024':('30','10')})
        if a.perf:perf(a,out)
    return 0 if summary(out) else 1

if __name__=='__main__':sys.exit(main())
