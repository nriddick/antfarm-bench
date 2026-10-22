"""Run one full pass: every model configuration × library through pipeline.py, K at a time.

Usage (detached): setsid nohup python3 -B results/unified/open-frame/run_pass.py WAVE [K] &
Each trajectory logs to .build/open-frame/<WAVE>/logs/<model>-<system>.log; PASS.json records
start/end/exit per trajectory. Evaluations serialize through the quiet lock, so K bounds only
how many producer pipelines are in flight. Order is shuffled with a recorded seed.
"""
import datetime,json,os,random,subprocess,sys,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
MODELS=['opus-high','opus-medium','sonnet-high','sonnet-medium']
SYSTEMS=['antfarm','tbb','rayon']  # Java FJP dropped (user decision 2026-10-21)

def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()

def main(wave,k='6'):
    k=int(k);seed=int(time.time());jobs=[(m,s) for m in MODELS for s in SYSTEMS];random.Random(seed).shuffle(jobs)
    logs=ROOT/'.build/open-frame'/wave/'logs';logs.mkdir(parents=True,exist_ok=True)
    out=HERE/wave;out.mkdir(parents=True,exist_ok=True)
    record={'wave':wave,'concurrency':k,'seed':seed,'order':jobs,'started':now(),'trajectories':{}}
    def save():(out/'PASS.json').write_text(json.dumps(record,indent=2)+'\n')
    running={};queue=list(jobs);save()
    env=dict(os.environ,AFBENCH_WAVE=wave);env.pop('AFBENCH_SCOPE',None)
    while queue or running:
        while queue and len(running)<k:
            m,s=queue.pop(0);name=f'{m}-{s}'
            log=(logs/f'{name}.log').open('a')
            running[name]=(subprocess.Popen([sys.executable,'-B','pipeline.py',m,s],cwd=HERE,env=env,stdout=log,stderr=subprocess.STDOUT),log)
            record['trajectories'][name]={'started':now()};save()
            time.sleep(20)  # stagger session starts
        for name,(proc,log) in list(running.items()):
            if proc.poll() is not None:
                log.close();del running[name]
                record['trajectories'][name].update(ended=now(),exit_code=proc.returncode);save()
        time.sleep(15)
    record['ended']=now();save()

if __name__=='__main__':main(*sys.argv[1:])
