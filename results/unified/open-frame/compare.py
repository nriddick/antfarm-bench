"""Cross-pass comparison from recorded evidence only. Usage: python3 -B compare.py OUTDIR pass1 pass2 ...
Writes OUTDIR/COMPARISON.md, COMPARISON.json and LIMITATIONS.md. Timings: the parent's split-smt high-count
evaluation of each trajectory's adapted source, per-trajectory medians of fresh plain processes, at the
worker counts common to all passes (8/15/30/40). Each cell reports the median across passes and the range;
one value per pass, so with five passes this describes run-to-run spread, not a significance test.
"""
import json,re,statistics,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
NS=(8,15,30,40);LIBS=('antfarm','tbb','rayon');MODELS=('opus-high','opus-medium','sonnet-high','sonnet-medium')
WORK=[('frame','frame-gol-heavy','composite heavy frame, ms',1e6),('frame','frame-gol-small','composite small frame, ms',1e6),
      ('native','1024','native GoL 1024², ms',1e6),('native','256','native GoL 256², ms',1e6),('native','64','native GoL 64², ms',1e6)]
KEYS=re.compile(r'(limitation|falls short|does not provide|doesn.t provide|no (?:supported|documented|library|built-in)|not (?:supported|exposed|available)|cannot|can.t)',re.I)

def load(p):
    try:return json.loads(Path(p).read_text())
    except (OSError,ValueError):return None

def adapt_values(t):
    vals={}
    for p in load(t/'eval-adapt'/'processes.json') or []:
        if p['variant']=='plain' and p['label']=='adapt' and p['passed'] and p.get('value_ns'):
            vals.setdefault((p['kind'],str(p['what']),p['workers']),[]).append(p['value_ns'])
    return {k:statistics.median(v) for k,v in vals.items()}

def main(outdir,*passes):
    out=HERE/outdir;out.mkdir(exist_ok=True);data={};lims=[]
    for w in passes:
        for t in sorted((HERE/w).iterdir()):
            if not t.is_dir() or t.name.count('-')<2:continue
            m,lib=t.name.rsplit('-',1)
            sel=load(t/'eval-opt1'/'SELECTION.json') or {}
            gates={g:load(t/f'gate-{g}'/'GATE.json') for g in ('opt1','adapt')}
            rep=(t/'reviewer-sonnet-medium'/'REPORT.md');disp=None
            if rep.exists():disp=next((l.replace('*','').split(':',1)[-1].strip().split('.')[0] for l in rep.read_text().splitlines() if 'isposition' in l),None)
            stages={s:load(t/s/'PROCESS.json') for s in ('initial','opt1','adapt')}
            data.setdefault((lib,m),[]).append({'pass':w,'values':adapt_values(t),'selection':sel.get('decision'),
                'gates':{g:(v['native'],v['frame']) if v else None for g,v in gates.items()},'review':disp,
                'minutes':sum(s['wall_seconds'] for s in stages.values() if s and 'wall_seconds' in s)/60})
            for s in ('initial','opt1','adapt'):
                r=t/s/'REPORT.md'
                if r.exists():
                    for line in r.read_text().splitlines():
                        if KEYS.search(line) and len(line)<400:lims.append((lib,m,w,s,line.strip()))
    fmt=lambda xs,d:'—' if not xs else (f'{statistics.median(xs)/d:.3g}' + (f' ({min(xs)/d:.3g}–{max(xs)/d:.3g})' if len(xs)>1 else ''))
    md=['# Cross-pass comparison','',f'Passes: {", ".join(passes)}. Adapted source, parent split-smt evaluation; median across passes (range). '
        'Each pass contributes one trajectory per cell. Pass 1 evaluated N=8/15/24/30/32/40, later passes N=8/15/30/40.','']
    for kind,what,title,div in WORK:
        md+= [f'## {title}','','| Library | Model | '+' | '.join(f'N={n}' for n in NS)+' |','|---|---|'+'---|'*len(NS)]
        for lib in LIBS:
            for m in MODELS:
                runs=data.get((lib,m),[])
                md.append(f'| {lib} | {m} | '+' | '.join(fmt([r['values'][(kind,what,n)] for r in runs if (kind,what,n) in r['values']],div) for n in NS)+' |')
        md.append('')
    md+=['## Process outcomes','','| Library | Model | passes | opt1 retained (native/frame) | gate fails | reviews | median producer minutes |','|---|---|---|---|---|---|---|']
    for lib in LIBS:
        for m in MODELS:
            runs=data.get((lib,m),[])
            ret=f"{sum(1 for r in runs if r['selection'] and r['selection'].get('native'))}/{sum(1 for r in runs if r['selection'] and r['selection'].get('frame'))}"
            gf=sum(1 for r in runs for g in r['gates'].values() if g and not all(g))
            rv='; '.join(sorted({r['review'] or '—' for r in runs}))
            md.append(f"| {lib} | {m} | {len(runs)} | {ret} | {gf} | {rv} | {statistics.median([r['minutes'] for r in runs]) if runs else 0:.0f} |")
    (out/'COMPARISON.md').write_text('\n'.join(md)+'\n')
    (out/'COMPARISON.json').write_text(json.dumps({f'{k[0]}|{k[1]}':[{**r,'values':{f'{a}|{b}|{c}':v for (a,b,c),v in r['values'].items()}} for r in v] for k,v in data.items()},indent=1)+'\n')
    (out/'LIMITATIONS.md').write_text('# Library-limitation statements in producer reports (keyword extract; read in context)\n\n'+
        '\n'.join(f'- **{lib} / {m} / {w} / {s}:** {line}' for lib,m,w,s,line in lims)+'\n')
    print(out/'COMPARISON.md')

if __name__=='__main__':main(*sys.argv[1:])
