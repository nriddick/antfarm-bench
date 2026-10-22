"""Library-use gate for a producer stage before retention (user decision 2026-10-21).

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/gate.py MODEL SYSTEM STAGE
Sonnet 5.5 medium reads the stage's final source and its diff against the stage start in an
isolated /tmp root and ends with `NATIVE: PASS|FAIL` and `FRAME: PASS|FAIL`. Output:
<trajectory>/gate-<stage>/GATE.json. An unparseable verdict counts as FAIL.
selection.py retains a workload only if it passes; disposition.py records adapt verdicts.
"""
import json,re,shutil,subprocess,sys
import agents as ag
from review import REVIEW_ROOT

GATE_MODEL='sonnet-medium'

def gate(model,system,stage):
    src=ag.OUT/f'{model}-{system}';rec=json.loads((src/stage/'PROCESS.json').read_text())
    base,cand=rec['base_commit'],rec['commit']
    root=REVIEW_ROOT/'open-frame'/ag.WAVE/f'{model}-{system}'/f'gate-{stage}';root.mkdir(parents=True)
    subprocess.run(['git','clone','-q',str(src/stage/'workspace.bundle'),str(root/'workspace')],check=True)
    subprocess.run(['git','checkout','-q',cand],cwd=root/'workspace',check=True)
    paths=['impl/',f'benchmarks/unified/microframe/{ag.FRAME[system]}',ag.POOL[system]]
    (root/'DIFF.patch').write_text(subprocess.check_output(['git','diff',base,cand,'--',*paths],cwd=root/'workspace',text=True))
    shutil.rmtree(root/'workspace/.git');subprocess.run(['git','init','-q','--template=',str(root)],check=True)
    dest=src/f'gate-{stage}';dest.mkdir()
    packet=(ag.ROOT/'docs/unified/prompts/gol-frame-gate.txt').read_text().replace('{library}',ag.LIBRARY[system])
    (dest/'packet.txt').write_text(packet)
    with ag.quiet(False):r=ag.run_native(GATE_MODEL,root,dest,packet,tools='Bash,Read,Glob,Grep')
    r=ag.retry_api(GATE_MODEL,root,dest,r,tools='Bash,Read,Glob,Grep')
    text=(dest/'REPORT.md').read_text()
    verdict={k:(m.group(1)=='PASS' if (m:=re.search(rf'^\W*{k}:\W*(PASS|FAIL)\W*$',text,re.M|re.I)) else None) for k in ('NATIVE','FRAME')}
    g={'stage':stage,'base_commit':base,'candidate_commit':cand,'model':GATE_MODEL,'exit_code':r['exit_code'],
       'native':verdict['NATIVE'] is True,'frame':verdict['FRAME'] is True,'parsed':{k:v is not None for k,v in verdict.items()},'report':'REPORT.md'}
    ag.write(dest/'GATE.json',g);print('gate',stage,'native',g['native'],'frame',g['frame'],flush=True)

if __name__=='__main__':gate(*sys.argv[1:4])
