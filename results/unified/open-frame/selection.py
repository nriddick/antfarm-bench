"""Independent ordinary selection after opt1, from a paired evaluation.

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/selection.py MODEL SYSTEM [EVAL=eval-opt1]

The evaluation's first label is the base, the second the candidate. A workload's candidate
is retained only if every candidate audit and plain process for it passed and the geometric
mean of its paired candidate/base ratios is below THRESHOLD (more than 1.5% faster). Native GoL
owns impl/; the frame adapter is judged on the composite frames only, while standalone opaque-C
recipes must still pass but are descriptive. If the shared hosting
helper differs between the two versions the decision is joint: all candidate files or none,
on the geomean over both workloads. The workspace receives the composed source and
SELECTION.json as one parent commit.
"""
import json,math,subprocess,sys
import agents as ag

# User decision 2026-10-21: 1.5% (was 1%); same-commit pairs differed by 0.7-1.2%.
THRESHOLD=0.985

def main(model,system,name='eval-opt1'):
    ws=ag.workspace(model,system);ev=ag.OUT/f'{model}-{system}'/name
    summary=json.loads((ev/'summary.json').read_text());(base,bc),(cand,cc)=list(summary['labels'].items())[:2]
    key=f'{cand}/{base}'
    def composite(c):return c['kind']=='native' or not c['what'].startswith('gol-c-')
    def workload(kind):
        # Correctness covers every recipe; the ratio rule uses native, or composite frames only.
        checked=[c for c in summary['configs'] if c['kind']==kind]
        rows=[c for c in checked if composite(c)]
        ratios=[x for c in rows for x in c[key]]
        correct=all(c[cand]['audit_ok'] and c[cand]['plain_ok'] for c in checked)
        complete=all(len(c[key])==len(c[cand]['values_ns']) and c[key] for c in rows)
        g=math.exp(sum(map(math.log,ratios))/len(ratios)) if ratios else None
        return {'geomean_ratio':g,'correct':correct,'complete_pairs':complete,'retain':bool(correct and complete and g is not None and g<THRESHOLD)}
    native,frame=workload('native'),workload('frame')
    gate_file=ag.OUT/f'{model}-{system}'/'gate-opt1'/'GATE.json'
    gate=json.loads(gate_file.read_text()) if gate_file.exists() else None
    # Library-use gate (user decision 2026-10-21): a failing workload is never retained.
    if gate:
        for name,w in (('native',native),('frame',frame)):
            w['library_use_pass']=gate[name];w['retain']=w['retain'] and gate[name]
    # Descriptive only; the frame decision uses composite frames (user decision 2026-10-21).
    for cat,pre in (('composite','frame-'),('opaque_c','gol-c-')):
        xs=[x for c in summary['configs'] if c['kind']=='frame' and c['what'].startswith(pre) for x in c[key]]
        frame[f'geomean_ratio_{cat}']=math.exp(sum(map(math.log,xs))/len(xs)) if xs else None
    groups={'native':['impl/'],'frame':[f'benchmarks/unified/microframe/{ag.FRAME[system]}']}
    pool=ag.POOL[system]
    changed=subprocess.check_output(['git','diff','--name-only',bc,cc],cwd=ws,text=True).split()
    joint=pool in changed
    if joint:
        rows=[x for c in summary['configs'] if composite(c) for x in c[key]]
        g=math.exp(sum(map(math.log,rows))/len(rows)) if rows else None
        # User decision 2026-10-21: a joint change also may not regress either workload beyond the threshold.
        no_regression=all(w['geomean_ratio'] is not None and w['geomean_ratio']<=1/THRESHOLD for w in (native,frame))
        keep=native['correct'] and frame['correct'] and native['complete_pairs'] and frame['complete_pairs'] and g is not None and g<THRESHOLD and no_regression
        if gate:keep=keep and gate['native'] and gate['frame']
        decision={'native':keep,'frame':keep,'hosting':keep,'joint_geomean_ratio':g,'joint_no_regression':no_regression}
    else:decision={'native':native['retain'],'frame':frame['retain'],'hosting':False}
    paths={'native':groups['native'],'frame':groups['frame'],'hosting':[pool]}
    for group,keep in ((g,decision[g]) for g in ('native','frame','hosting')):
        for path in paths[group]:
            subprocess.run(['git','rm','-r','-q','--ignore-unmatch','--',path],cwd=ws,check=True)
            subprocess.run(['git','checkout',cc if keep else bc,'--',path],cwd=ws,check=True)
    selection={'evaluation':name,'base':{base:bc},'candidate':{cand:cc},'rule':f'retain when all candidate processes pass and the paired geomean ratio < {THRESHOLD} over native or composite-frame configurations (opaque-C descriptive); joint when the hosting helper changed, and then neither workload may exceed {1/THRESHOLD:.4f}',
               'changed_files':changed,'joint':joint,'library_use_gate':gate,'native':native,'frame':frame,'decision':decision,
               'selected_source':{g:(cand if decision[g] else base) for g in ('native','frame','hosting')}}
    ag.write(ws/'SELECTION.json',selection);ag.write(ev/'SELECTION.json',selection)
    ag.commit(ws,'Independent selection after opt1');print(json.dumps(selection['decision']),ag.git(ws,'rev-parse','HEAD'))

if __name__=='__main__':main(*sys.argv[1:])
