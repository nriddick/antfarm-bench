"""Isolated review of one trajectory (all completed stages and parent evaluations).

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/review.py MODEL SYSTEM [REVIEWER]
REVIEWER: luna-high (codex gpt-6-luna, high) or a Claude key from agents.MODELS such as
sonnet-medium (read/search/shell tools only). Output: reviewer-<REVIEWER>/ (dry runs 1-2
kept their first Luna review in reviewer/).
The review root lives outside the parent checkout (default /tmp/afbench-review) and exposes
only this trajectory: the workspace at its final commit with its evidence, each stage's
packet/report/transcript, and the parent's evaluations and selection. Dry run 1 used an
earlier single-stage version of this script (its executed copy is not retained separately;
see git history).
"""
import json,os,shutil,subprocess,sys,time
from pathlib import Path
import agents as ag

# Outside the parent checkout so no repository instructions or handoffs are inherited.
REVIEW_ROOT=Path(os.environ.get('AFBENCH_REVIEW_ROOT','/tmp/afbench-review'))

def review(model,system,reviewer='luna-high'):
    src=ag.OUT/f'{model}-{system}';ws=ag.workspace(model,system)
    stages=[s for s in ag.STAGES if (src/s/'PROCESS.json').exists()]
    root=REVIEW_ROOT/'open-frame'/ag.WAVE/f'{model}-{system}'/reviewer;root.mkdir(parents=True)
    subprocess.run(['git','clone','-q',str(src/stages[-1]/'workspace.bundle'),str(root/'workspace')],check=True)
    subprocess.run(['git','checkout','-q',json.loads((src/stages[-1]/'PROCESS.json').read_text())['commit']],cwd=root/'workspace',check=True)
    if (ws/'evidence').exists():shutil.copytree(ws/'evidence',root/'workspace/evidence',ignore=shutil.ignore_patterns('*.o','*.a','*.so','*.class'))
    for stage in stages:
        d=root/'stages'/stage;d.mkdir(parents=True)
        shutil.copy2(src/stage/'packet.txt',d/'PROMPT.txt');shutil.copy2(src/stage/'REPORT.md',d/'REPORT.md')
        record=json.loads((src/stage/'PROCESS.json').read_text());ag.write(d/'STAGE.json',{k:record.get(k) for k in ('commit','exit_code','wall_seconds','protected_inputs_unchanged','changed_protected','added_files','record_correction','test_runs','native_models')})
        native=[p for p in (src/stage).glob('*.jsonl') if p.name!='events.jsonl']
        if native:shutil.copy2(native[0],d/'transcript.jsonl')
    for gate_dir in sorted(src.glob('gate-*')):
        shutil.copytree(gate_dir,root/'parent-evaluations'/gate_dir.name,ignore=shutil.ignore_patterns('*.jsonl'))
    for ev in sorted(src.glob('eval-*')):
        shutil.copytree(ev,root/'parent-evaluations'/ev.name,ignore=shutil.ignore_patterns('*.o','*.a','*.tar','build-*','raw.tar.gz'))
        if (ev/'raw.tar.gz').exists():
            import tarfile
            with tarfile.open(ev/'raw.tar.gz') as t:t.extractall(root/'parent-evaluations'/ev.name,filter='data')
    subprocess.run(['git','init','-q','--template=',str(root)],check=True)
    dest=src/f'reviewer-{reviewer}';dest.mkdir()
    packet=(ag.ROOT/'docs/unified/prompts/gol-frame-review.txt').read_text().replace('{library}',ag.LIBRARY[system])
    (dest/'packet.txt').write_text(packet)
    if reviewer!='luna-high':
        with ag.quiet(False):r=ag.run_native(reviewer,root,dest,packet,tools='Bash,Read,Glob,Grep')
        r=ag.retry_api(reviewer,root,dest,r,tools='Bash,Read,Glob,Grep')
        r['root']=str(root);r['stages']=stages
        ag.write(dest/'PROCESS.json',r);print('review',reviewer,r['exit_code'],round(r['wall_seconds'],1),flush=True);return
    argv=['codex','exec','--ignore-user-config','--model','gpt-6-luna','-c','model_reasoning_effort="high"','--sandbox','workspace-write',
          '--skip-git-repo-check','-C',str(root),'--json','-o',str(dest/'REPORT.md'),'-']
    r={'argv':argv,'root':str(root),'stages':stages};start=time.monotonic()
    with (dest/'events.jsonl').open('w') as o,(dest/'stderr.log').open('w') as e,ag.quiet(False):
        proc=subprocess.run(argv,cwd=root,input=packet,stdout=o,stderr=e,text=True)
    r.update(exit_code=proc.returncode,wall_seconds=time.monotonic()-start)
    events=[json.loads(l) for l in (dest/'events.jsonl').read_text().splitlines() if l.strip()]
    thread=next((e['thread_id'] for e in events if e.get('type')=='thread.started'),None);r['session_id']=thread
    matches=list((Path(os.environ['CODEX_HOME'])/'sessions').rglob(f'*{thread}*.jsonl')) if thread else []
    if len(matches)==1:
        shutil.copy2(matches[0],dest/matches[0].name)
        r['native_identity']=[{'model':x['payload'].get('model'),'effort':x['payload'].get('effort')} for l in matches[0].read_text().splitlines() if (x:=json.loads(l)).get('type')=='turn_context']
    ag.write(dest/'PROCESS.json',r);print('review',r['exit_code'],round(r['wall_seconds'],1),flush=True)

if __name__=='__main__':review(*sys.argv[1:4])
