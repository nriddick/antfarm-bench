"""Drive one trajectory through every stage; stops at the first failing step.

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/pipeline.py MODEL SYSTEM [FROM_STEP]
Steps: initial, eval-initial, opt1, gate-opt1, eval-opt1, selection, adapt, gate-adapt, eval-adapt, disposition, review.
Completed steps (their output directory exists) are skipped, so a rerun resumes.
"""
import json,os,subprocess,sys
import agents as ag

HIGH=['8','15','24','30','32','40']
STAGE_STEPS=('initial','opt1','adapt')
# Parent high-count evaluation counts (user decision 2026-10-21, from pass 2); agents still see all of HIGH.
EVAL_HIGH=['8','15','30','40']
# Paired fresh processes per configuration (user decision 2026-10-21: more than the dry-run 2).
PAIRS='4'
# Reviewer trial (user 2026-10-21): Sonnet 5.5 medium; luna-high remains available.
REVIEWER=os.environ.get('AFBENCH_REVIEWER','sonnet-medium')

def commit(model,system,stage):return json.loads((ag.OUT/f'{model}-{system}'/stage/'PROCESS.json').read_text())['commit']

def main(model,system,start='initial'):
    here=ag.HERE;out=ag.OUT/f'{model}-{system}'
    def py(*args):subprocess.run([sys.executable,'-B',*map(str,args)],cwd=here,check=True)
    steps=[
        ('initial',lambda:py('agents.py',model,system,'initial')),
        ('eval-initial',lambda:py('evaluate.py',model,system,'eval-initial','--labels',f'initial={commit(model,system,"initial")}','--reps','1')),
        ('opt1',lambda:py('agents.py',model,system,'opt1')),
        ('gate-opt1',lambda:py('gate.py',model,system,'opt1')),
        ('eval-opt1',lambda:py('evaluate.py',model,system,'eval-opt1','--labels',f'initial={commit(model,system,"initial")}',f'opt1={commit(model,system,"opt1")}','--reps',PAIRS)),
        ('selection',lambda:py('selection.py',model,system)),
        ('adapt',lambda:py('agents.py',model,system,'adapt')),
        ('gate-adapt',lambda:py('gate.py',model,system,'adapt')),
        ('eval-adapt',lambda:py('evaluate.py',model,system,'eval-adapt','--labels',f'selected={json.loads((out/"adapt"/"PROCESS.json").read_text())["base_commit"]}',
                                f'adapt={commit(model,system,"adapt")}','--placement','split-smt','--workers',*EVAL_HIGH,'--reps',PAIRS)),
        ('disposition',lambda:py('disposition.py',model,system)),
        ('review',lambda:py('review.py',model,system,REVIEWER)),
    ]
    names=[s for s,_ in steps];todo=steps[names.index(start):]
    for name,step in todo:
        done=((name in STAGE_STEPS and (out/name/'PROCESS.json').exists() and 'commit' in json.loads((out/name/'PROCESS.json').read_text()))
              or (name not in STAGE_STEPS and name!='review' and (out/name/'summary.json').exists()) or (name=='review' and (out/f'reviewer-{REVIEWER}'/'PROCESS.json').exists()) or (out/name/'summary.json').exists() or (name=='selection' and (out/'eval-opt1'/'SELECTION.json').exists())
              or (name=='disposition' and (out/'eval-adapt'/'DISPOSITION.json').exists())
              or (name.startswith('gate-') and (out/name/'GATE.json').exists()))
        if done:print('skip',name,flush=True);continue
        if name in STAGE_STEPS and (out/name/'PROCESS.json').exists():
            raise SystemExit(f'{name} was interrupted; run agents.py resume {model} {system} {name} first')
        print('step',name,flush=True);step()

if __name__=='__main__':main(*sys.argv[1:])
