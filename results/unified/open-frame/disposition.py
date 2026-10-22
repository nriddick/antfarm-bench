"""Record the parent's disposition of the adapt stage from eval-adapt (no automatic replacement).

Usage: AFBENCH_WAVE=W python3 -B results/unified/open-frame/disposition.py MODEL SYSTEM
Writes eval-adapt/DISPOSITION.json: correctness of both versions per configuration, failures
or startup failures (no timing), per-workload geomeans, and every configuration where the
adapted version is more than 1.5% slower (the selection threshold). Both sources stay recorded; neither replaces the other.
"""
import json,math,sys
import agents as ag

def main(model,system,name='eval-adapt'):
    ev=ag.OUT/f'{model}-{system}'/name;s=json.loads((ev/'summary.json').read_text())
    (base,_),(cand,_)=list(s['labels'].items())[:2];key=f'{cand}/{base}'
    def gm(xs):return math.exp(sum(map(math.log,xs))/len(xs)) if xs else None
    rows=[]
    for c in s['configs']:
        r=c[key];rows.append({'kind':c['kind'],'what':c['what'],'workers':c['workers'],'descriptive':c['kind']=='frame' and c['what'].startswith('gol-c-'),
            'correct':{l:c[l]['audit_ok'] and c[l]['plain_ok'] for l in (base,cand)},'pairs':len(r),'geomean_ratio':gm(r)})
    d={'evaluation':name,'labels':s['labels'],'placement':s['placement'],
       'rule':'descriptive: both versions stay recorded; regressions >1.5% and failures listed; no automatic replacement',
       'failures':s['failures'],
       'all_correct':{l:all(r['correct'][l] for r in rows) for l in (base,cand)},
       # The composite frames alone measure the frame adapter; opaque-C is descriptive.
       'geomean':{'native':gm([x for c in s['configs'] if c['kind']=='native' for x in c[key]]),
                  'frame':gm([x for c in s['configs'] if c['kind']=='frame' and not c['what'].startswith('gol-c-') for x in c[key]])},
       # Separate quantities: composite frames vs standalone opaque-C GoL recipes.
       'geomean_by_category':{cat:gm([x for c in s['configs'] if c['kind']=='frame' and c['what'].startswith(pre) for x in c[key]]) for cat,pre in (('composite','frame-'),('opaque_c','gol-c-'))},
       'regressions_over_1_5pct':[r for r in rows if r['geomean_ratio'] and r['geomean_ratio']>1.015],
       'missing_pairs':[r for r in rows if r['pairs']==0],'configs':rows}
    gate_file=ag.OUT/f'{model}-{system}'/'gate-adapt'/'GATE.json'
    d['library_use_gate']=json.loads(gate_file.read_text()) if gate_file.exists() else None
    if d['library_use_gate']:d['acceptable_paths']={k:d['library_use_gate'][k] for k in ('native','frame')}
    ag.write(ev/'DISPOSITION.json',d);print(json.dumps({k:d[k] for k in ('all_correct','geomean','geomean_by_category')}),len(d['regressions_over_1_5pct']),'regressions')

if __name__=='__main__':main(*sys.argv[1:3])
