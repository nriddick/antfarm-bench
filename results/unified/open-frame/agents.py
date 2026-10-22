"""Open-hosting GoL+frame sessions: neutral trees, compact packet, retained transcripts.

Usage: AFBENCH_WAVE=dryrun1 python3 -B results/unified/open-frame/agents.py MODEL SYSTEM [initial|opt1|adapt]
Sequence: initial -> evaluate.py eval-initial (initial only) -> opt1 -> evaluate.py eval-opt1
(initial vs opt1) -> selection.py -> adapt -> evaluate.py eval-adapt (split-smt) -> review.py.
MODEL: opus-high | opus-medium | sonnet-high | sonnet-medium. SYSTEM: antfarm | tbb | rayon | fjp.
Workspaces live in .build/open-frame/<wave>/<model>-<system>; evidence in <wave>/<model>-<system>/<stage>.
Existing workspaces/evidence are never overwritten.
"""
import datetime,hashlib,json,os,shutil,subprocess,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];HERE=Path(__file__).resolve().parent
WAVE=os.environ.get('AFBENCH_WAVE','dryrun1')
OUT=HERE/WAVE
PAYLOAD=Path(os.environ.get('AFBENCH_PAYLOAD',HERE/'payload'))
sys.path.insert(0,str(ROOT/'scripts/unified'))
import mkbundle

PIN=dict(commit='5f257e5ab894307be6e1e73f4c64fdb23743a085',branch='1.9.1-progress',tag='afbench-dev-5f257e5')
MODELS={'opus-high':('claude-opus-5-5','high'),'opus-medium':('claude-opus-5-5','medium'),
        'sonnet-high':('claude-sonnet-5-5','high'),'sonnet-medium':('claude-sonnet-5-5','medium')}
LIBRARY={'antfarm':'Antfarm','tbb':'oneTBB','rayon':'Rayon','fjp':'Java ForkJoinPool'}
LANG={'antfarm':'d','tbb':'cpp','rayon':'rust','fjp':'java'}
FRAME={'antfarm':'antfarm_frame.d','tbb':'tbb.cpp','rayon':'rayon_frame.rs','fjp':'FrameImpl.java'}
POOL={'antfarm':'benchmarks/unified/bindings/d/pool_antfarm.d','tbb':'benchmarks/unified/bindings/cpp/pool_tbb.hpp',
      'rayon':'benchmarks/unified/bindings/rust/afcore-sys/src/pool_rayon.rs','fjp':'benchmarks/unified/bindings/java/afbench/core/PinnedPool.java'}

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

from contextlib import contextmanager
QUIET=ROOT/'.build/open-frame/quiet.lock';TURNSTILE=ROOT/'.build/open-frame/quiet-turnstile.lock'
@contextmanager
def quiet(exclusive):
    """Agent sessions hold this shared; parent evaluations hold it exclusively, so builds and
    measurements happen only while no producer or reviewer is active (concurrent trajectories).
    Writer preference: everyone passes the turnstile to acquire; an evaluation keeps holding it
    while it waits, so no new session starts until the evaluation has the lock."""
    import fcntl
    QUIET.parent.mkdir(parents=True,exist_ok=True);QUIET.touch(exist_ok=True);TURNSTILE.touch(exist_ok=True)
    with QUIET.open('rb') as f,TURNSTILE.open('rb') as t:
        fcntl.flock(t,fcntl.LOCK_EX)
        try:fcntl.flock(f,fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        finally:fcntl.flock(t,fcntl.LOCK_UN)
        try:yield
        finally:fcntl.flock(f,fcntl.LOCK_UN)
def write(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def git(ws,*args):return subprocess.check_output(['git',*args],cwd=ws,text=True).strip()
def commit(ws,msg):
    subprocess.run(['git','add','-A'],cwd=ws,check=True)
    subprocess.run(['git','-c','user.name=Benchmark','-c','user.email=bench@local','-c','commit.gpgsign=false','commit','--allow-empty','-qm',msg],cwd=ws,check=True)

def workspace(model,system):return ROOT/'.build/open-frame'/WAVE/f'{model}-{system}'
# AFBENCH_SCOPE=native limits a later stage to native GoL: the frame adapter becomes protected.
SCOPE=os.environ.get('AFBENCH_SCOPE','both')
def editable(system):return ['impl/',*([] if SCOPE=='native' else [f'benchmarks/unified/microframe/{FRAME[system]}']),POOL[system],'NOTES.md']

def inputs(ws,system):
    skip=editable(system)
    return {str(p.relative_to(ws)):sha(p) for p in ws.rglob('*') if p.is_file()
            and not any(x in p.relative_to(ws).parts for x in ('.git','.build','evidence','__pycache__','target'))
            and str(p.relative_to(ws)) not in ('frame/Cargo.toml','frame/Cargo.lock')
            and not any(str(p.relative_to(ws)).startswith(e) for e in skip)}

CARDS={
'antfarm':"""- Native (`impl/gol_stub.d`): a class implementing `afcore.GolImpl` — `int start(ref const AfRunCfg)`,
  `int load(const(ubyte)* grid, uint w, uint h)`, `int run(uint generations)`, `int store(ubyte* out)`, `int stop()`;
  entry `mixin golMain!YourClass;`. Audit hook `auditCell(gen, cell, next)` (module `afcore`; compiled out in plain builds).
- Frame (`benchmarks/unified/microframe/antfarm_frame.d`): `extern(C) nothrow` `int start(const(AfRunCfg)*)`,
  `int frame(MfFrame*)`, `void stop()`; bridge module `microframe`: `mf_batch_count(f)`, `mf_batch_shape(f, b, &shape)`,
  `mf_execute(f, b, wave, begin, end)`.
- Hosting helper (`benchmarks/unified/bindings/d/pool_antfarm.d`): `AntfarmPool.create(cfg, optionsHook)` builds a FarmHost
  with N workers pinned to `worker_cpus` and registers them; `publishRaw`, `host`, `close`. A starting point only.
- Build: `ldc2 -O2 -release -i` over `vendor/antfarm` (Antfarm `5f257e5`); druntime runs `gcopt=parallel:0`.
  Library material: `library-docs/` (guides, API docs) and the `vendor/antfarm` source.""",
'tbb':"""- Native (`impl/gol_stub.cpp`): a class deriving `af::GolImpl` (`afcore.hpp`) — `int start(const AfRunCfg&)`,
  `int load(const uint8_t* grid, uint32_t w, uint32_t h)`, `int run(uint32_t generations)`, `int store(uint8_t* out)`,
  `int stop()`; entry `AF_GOL_MAIN(YourClass)`. Audit hook `AF_AUDIT_CELL(gen, cell, next)` (compiled out in plain builds).
- Frame (`benchmarks/unified/microframe/tbb.cpp`): `static int start(const AfRunCfg*)`, `static int frame(MfFrame*)`,
  `static void stop()`; bridge `microframe.h`: `mf_batch_count`, `mf_batch_shape`, `mf_execute(f, b, wave, begin, end)`.
- Hosting helper (`benchmarks/unified/bindings/cpp/pool_tbb.hpp`): `af::TbbPool(cfg)` — a `task_arena` of N with workers
  registered by an observer; `run(callback)`. `af_tbb_for.hpp` has a small `parallel_for` wrapper. A starting point only.
- Build: `clang++ -std=c++20 -O2 -DNDEBUG -ltbb` against the installed oneTBB (`/usr/include/oneapi/tbb`).
  Library material: `library-docs/` and the installed oneTBB headers.""",
'rayon':"""- Native (`impl/main.rs`): a `Default` type implementing `afcore_sys::GolImpl` — `start(&mut self, &AfRunCfg)`,
  `load(&mut self, grid: &[u8], w, h)`, `run(&mut self, generations)`, `store(&mut self, out: &mut [u8])`, `stop(&mut self)`,
  each returning `c_int`; `main` returns `afcore_sys::gol_main::<YourType>()`. Audit hook `afcore_sys::audit_cell(gen, cell, next)`.
- Frame (`benchmarks/unified/microframe/rayon_frame.rs`): `pub unsafe fn start(&AfRunCfg) -> i32`,
  `pub unsafe fn frame(*mut MfFrame) -> i32`, `pub unsafe fn stop()`; bridge `crate::{MfFrame, MfBatchShape, mf_batch_count,
  mf_batch_shape, mf_execute}`.
- Hosting helper (`benchmarks/unified/bindings/rust/afcore-sys/src/pool_rayon.rs`): `RayonPool::new(cfg)` builds N Rayon
  threads registered in `start_handler`; `run(closure)`. A starting point only.
- Build: `cargo build --release --locked --offline` with vendored crates (rayon 1.10.0, rayon-core 1.12.1).
  Library material: `library-docs/` (crate sources and docs).""",
'fjp':"""- Native (`impl/GolStub.java`), frame (`benchmarks/unified/microframe/FrameImpl.java`, bridge `FrameMain.batchCount/batchShape/execute`),
  hosting helper `benchmarks/unified/bindings/java/afbench/core/PinnedPool.java`. JDK 25 language level, G1, 2 GiB heap.
  Library material: `library-docs/` and the installed JDK documentation.""",
}

def placement_facts():
    import topology,microframe as mf
    p=topology.discover();ctl=p['policies']['phys']['control_cpu']
    doms={}
    for c in p['cores']:doms.setdefault(c['l3_domain'],[]).append(c['primary'])
    sib={c['primary']:[x for x in c['siblings'] if x!=c['primary']] for c in p['cores']}
    return (f"{len(p['cores'])} cores / {sum(len(c['siblings']) for c in p['cores'])} logical CPUs; L3 domains "
            + '; '.join(f"{d}: cores {v[0]}-{v[-1]}" for d,v in sorted(doms.items()))
            + f"; SMT sibling of CPU c is c+{sib[0][0]-0 if sib.get(0) else '?'}. Control CPU {ctl} (and its sibling) is reserved. "
            + f"phys placement: N=8 uses CPUs {mf.placement(p,'phys',8)} (one L3 domain); N=15 uses {mf.placement(p,'phys',15)} (both).")

def run_md(system,ws):
    return f"""# RUN.md — {LIBRARY[system]} GoL and composite frames

Everything you need is here; the scripts under `scripts/`, `benchmarks/unified/core/` and the bindings need not be read.

## Workspace

- Editable: `impl/` (native GoL), `benchmarks/unified/microframe/{FRAME[system]}` (frame adapter), `{POOL[system]}`
  (hosting glue), `NOTES.md`. Everything else is protected input.
- `library-docs/`: pinned library documentation. `evidence/`: your outputs. `.build/`: binaries.
- Machine: {placement_facts()}

## Interfaces

{CARDS[system]}
- Registration (`afcore.h`, all languages): `af_worker_enter(i)` pins to `worker_cpus[i]`; `af_worker_adopt()` and
  `af_worker_register(i or AF_WORKER_AUTO)` keep the current affinity; `af_workers_rendezvous(deadline_ns)` waits for N.
- `cfg.knobs_json` is `{{"grain":G}}` (G=0 means your default; positive G is a cell count, round as you document).
  `cfg.wait_mode` is `AF_WAIT_NATIVE` and prescribes nothing.

## Contract (checked by the C harness)

- Exactly N = `cfg.workers` threads are registered when `start` returns and stay alive; registration then closes.
  Only registered threads may compute cells or frame payload. Library helper threads may exist but must not compute.
- Allocation = the distinct CPUs in `cfg.worker_cpus` (control CPU excluded). Pin, widen or migrate within it; at the end
  each registered thread's affinity must lie inside it. `worker_cpus` is only a suggested slot map.
- The caller starts on the control CPU. It may orchestrate only (unregistered, no computing) or compute as one of the N:
  move its affinity into the allocation, register during `start`, and run N-1 other workers.
- Native: `load` resets to generation 0; timed `run(g)` continues the state and returns when complete; `store` writes it.
  Report each computed cell on its computing thread with the audit hook (gen = absolute generation since load).
- Frames: a frame is independent batches; batch b runs `waves` sequential waves of `items` items. Wave w may start only
  after wave w-1 of the same batch has completed; batches may overlap. `frame()` returns only when all work is done.
  `mf_batch_shape` gives kind (M1, M4 mixed cost, M6 dependent waves, GoL), items, waves, grain hint and GoL width.
  The composite recipes `frame-gol-small`/`frame-gol-heavy` judge the adapter; `gol-c-64/256/1024` are diagnostic.
  The payload is a prebuilt object linked unchanged.

## Testing budget (per stage)

Avoid exhaustive testing. Make the changes you are confident in, then:
1. one correctness test: `python3 -B scripts/unified/stage.py check --system {system} --out evidence/NAME`
   (plain and audit builds; native check/verify at 64/256/1024 and audits at 64/256, a short plain and audit run of
   every frame recipe; results compared with trusted references, audits also check your cell reports, reporting
   threads, coverage and wave order). If it fails, fix the fault and rerun it.
2. one instrumented run: `python3 -B scripts/unified/stage.py measure --system {system} --out evidence/NAME [--perf]`
   (native timing at 64/256/1024 and frame timing; `--perf` adds per-thread `perf record` reports of native 1024 and
   frame-gol-heavy at the largest N; profiles include untimed startup and reference replay on the control thread).
After the instrumented run, only adjust what you hypothesized or revert part of your changes; do not test again.
Building is not testing. Later stages may first run one `measure` of the code they start from. Defaults: phys placement,
N = 8 and 15 (`--placement`, `--workers` when a stage reveals more). The parent audits and evaluates the result itself.

Outputs: `evidence/NAME/SUMMARY.json` (one line per process: pass, native ms or frame p50 µs, participating slots) and
per-runner directories with JSONL records that include placement (masks, last CPU, caller slot) and audit participation.
Output directories must be new. Runners take the exclusive host lock; never wrap them in `flock`. If a protected input or
the harness is at fault, report it with evidence instead of editing it.
"""

def prepare(model,system):
    ws=workspace(model,system)
    if ws.exists():return ws
    if not (PAYLOAD/'manifest.json').exists():raise SystemExit(f'no prebuilt payload in {PAYLOAD}; run scripts/unified/build_payload.py --out {PAYLOAD}')
    ws.mkdir(parents=True);lang=LANG[system]
    for name in ('scripts/unified/afbuild.py','scripts/unified/topology.py','scripts/unified/microframe.py','scripts/unified/gol_frame.py','scripts/unified/stage.py'):
        dest=ws/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,dest)
    # Trusted core (reference, audit, registry, driver) ships prebuilt; no core/src in workspaces.
    shutil.copytree(ROOT/'benchmarks/unified/core/include',ws/'benchmarks/unified/core/include')
    shutil.copytree(PAYLOAD/'core',ws/'benchmarks/unified/core/prebuilt')
    shutil.copy2(ROOT/'benchmarks/unified/core/build_core.py',ws/'benchmarks/unified/core/build_core.py')
    shutil.copytree(ROOT/'benchmarks/unified/bindings'/lang,ws/'benchmarks/unified/bindings'/lang,ignore=shutil.ignore_patterns('stub','__pycache__'))
    frame=ws/'benchmarks/unified/microframe';frame.mkdir()
    names=['microframe.h','serial.c',*[p.name for p in (ROOT/'benchmarks/unified/microframe').glob('*.json')]]
    names+={'antfarm':['microframe.d','entry_antfarm.d','antfarm_frame.d'],'tbb':[],'rayon':['rayon_frame.rs','frame_rust.rs'],'fjp':['FrameImpl.java','FrameMain.java','frame_ffi.c']}[system]
    for name in names:shutil.copy2(ROOT/'benchmarks/unified/microframe'/name,frame/name)
    shutil.copytree(PAYLOAD,frame/'payload',ignore=shutil.ignore_patterns('core'))
    stub={'antfarm':'d/stub','tbb':'cpp/stub','rayon':'rust/afcore-sys/stub/impl','fjp':'java/stub'}[system]
    shutil.copytree(ROOT/'benchmarks/unified/bindings'/stub,ws/'impl')
    if system=='antfarm':
        mkbundle.write_antfarm_vendor(ws/'vendor');write(ws/'ANTFARM-PIN.json',PIN)
    if system=='rayon':
        import vendor_rust,afbuild
        vendor_rust.vendor(ws)
        shutil.copy2(ROOT/'benchmarks/unified/bindings/rust/afcore-sys/stub/Cargo.lock',ws/'Cargo.lock')
        (ws/'Cargo.toml').write_text(afbuild.recipe(system,ws/'impl',ws/'benchmarks/unified/bindings',ws/'.build/gol-frame/core',ws/'.build/gol-frame/plain')['manifest'])
    if system=='tbb':
        (frame/'tbb.cpp').write_text('#include "microframe.h"\n#include "pool_tbb.hpp"\nstatic int start(const AfRunCfg*) {return AF_ERROR;}\nstatic int frame(MfFrame*) {return AF_ERROR;}\nstatic void stop() {}\nint main(int argc,char**argv) {MfAdapter a={"tbb",start,frame,stop};return mf_main(argc,argv,&a); }\n')
    mkbundle.write_bundle(system,ws/'library-docs')
    # The higher-count phase is revealed later: initial runners offer phys placement only.
    p=ws/'scripts/unified/microframe.py';s=p.read_text();a=s.index('    if not 1<=workers<=40:');b=s.index('\ndef main():',a)
    s=s[:a]+"    raise ValueError('only phys supplied at this stage')\n"+s[b:];p.write_text(s.replace("choices=['phys','split-smt']","choices=['phys']"))
    p=ws/'scripts/unified/gol_frame.py';p.write_text(p.read_text().replace("choices=['phys','split-smt']","choices=['phys']"))
    (ws/'.gitignore').write_text('.build/\nevidence/\n__pycache__/\ntarget/\nframe/\n')
    (ws/'RUN.md').write_text(run_md(system,ws))
    subprocess.run(['git','init','-q','--template=',str(ws)],check=True);commit(ws,'Neutral GoL and frame packet')
    return ws

def retain_evidence(ws,dest,stage='initial'):
    """Archive the producer's git-ignored evidence/ as DEST/evidence.tar.gz, without compiled
    binaries or parent-supplied records; later stages skip paths an earlier stage archived."""
    import tarfile
    earlier=set()
    for s in STAGES:
        t=dest.parent/s/'evidence.tar.gz'
        if s!=stage and t.exists():
            with tarfile.open(t) as f:earlier|=set(f.getnames())
    src=ws/'evidence'
    if not src.exists():return
    with tarfile.open(dest/'evidence.tar.gz','w:gz') as out:
        for p in sorted(src.rglob('*')):
            if not p.is_file() or p.suffix in ('.o','.a','.so','.class') or p.open('rb').read(4)==b'\x7fELF':continue
            rel=str(Path('evidence')/p.relative_to(src))
            if p.relative_to(src).parts[0].startswith('independent-') or rel in earlier:continue
            out.add(p,arcname=rel)

def supply(ws,src,name):
    """Copy a parent evaluation's compact records into the producer's evidence/NAME."""
    target=ws/'evidence'/name;target.mkdir(parents=True)
    for f in ('summary.json','processes.json','metadata.json'):shutil.copy2(src/f,target/f)
    write(target/'ORIGIN.json',{'source':'parent independent paired evaluation of your own frozen code','sha256':{f:sha(src/f) for f in ('summary.json','processes.json','metadata.json')}})

STAGES={'initial':'gol-frame-initial.txt','opt1':'gol-frame-opt.txt','adapt':'gol-frame-adapt.txt'}

def launch(model,system,stage='initial'):
    assert stage in STAGES,stage
    ws=prepare(model,system) if stage=='initial' else workspace(model,system)
    trajectory=OUT/f'{model}-{system}';dest=trajectory/stage;dest.mkdir(parents=True)
    if stage=='opt1':
        shutil.copy2(trajectory/'initial'/'packet.txt',ws/'TASK.md')
        supply(ws,trajectory/'eval-initial','independent-initial')
        commit(ws,'Supply the original task text')
    if stage=='adapt':
        assert (ws/'SELECTION.json').exists(),'run selection.py first'
        # The later reveal: unrestricted parent runners (split-smt placement).
        for rel in ('scripts/unified/microframe.py','scripts/unified/gol_frame.py','scripts/unified/stage.py'):shutil.copy2(ROOT/rel,ws/rel)
        supply(ws,trajectory/'eval-opt1','independent-opt1')
        commit(ws,'Reveal split-smt placement for the higher-count phase')
    prompt=STAGES[stage].replace('.txt','-native.txt') if SCOPE=='native' and stage=='adapt' else STAGES[stage]
    packet=(ROOT/'docs/unified/prompts'/prompt).read_text().replace('{library}',LIBRARY[system]).replace('{workspace}',str(ws))
    (dest/'packet.txt').write_text(packet);shutil.copy2(ws/'RUN.md',dest/'RUN.md')
    before=inputs(ws,system);write(dest/'INPUTS.json',before);base_commit=git(ws,'rev-parse','HEAD')
    write(dest/'BASE.json',{'base_commit':base_commit})
    with quiet(False):record=run_native(model,ws,dest,packet)
    record=retry_api(model,ws,dest,record)
    finish(model,system,stage,record,base_commit)

def test_runs(dest):
    """Count the producer's test invocations from its tool calls (all attempts, including interrupted ones)."""
    import re
    counts={'stage_check':0,'stage_measure':0,'stage_measure_perf':0,'direct_runner':0,'direct_perf':0}
    for events in sorted(dest.glob('*events.jsonl')):
        for line in events.read_text().splitlines():
            e=json.loads(line)
            if e.get('type')!='assistant':continue
            for b in e['message'].get('content',[]):
                if b.get('type')!='tool_use' or b['name']!='Bash':continue
                c=b['input'].get('command','')
                for m in re.finditer(r'stage\.py\s+(check|measure)([^\n;&|]*)',c):
                    if '--help' in m.group(2) or '-h ' in m.group(2)+' ':continue
                    counts['stage_'+m.group(1)]+=1
                    if m.group(1)=='measure' and '--perf' in m.group(2):counts['stage_measure_perf']+=1
                counts['direct_runner']+=len(re.findall(r'python3?\b[^\n;&|]*?(?:gol_frame|microframe)\.py',c))
                counts['direct_perf']+=len(re.findall(r'(?:^|[\s;&|])perf\s+(?:record|stat)',c))
    return counts

def finish(model,system,stage,record,base_commit):
    ws=workspace(model,system);dest=OUT/f'{model}-{system}'/stage
    before=json.loads((dest/'INPUTS.json').read_text())
    record['base_commit']=base_commit
    after=inputs(ws,system)
    # Protected inputs are the files present before the session: modified or deleted ones fail the check.
    # Files the producer added outside the editable paths are listed but do not alter supplied inputs.
    record['changed_protected']=sorted(k for k in before if before[k]!=after.get(k))
    record['protected_inputs_unchanged']=not record['changed_protected']
    record['added_files']=sorted(set(after)-set(before))
    record['test_runs']=test_runs(dest);record['scope']=SCOPE
    commit(ws,f'Own {stage} output');record['commit']=git(ws,'rev-parse','HEAD');write(dest/'PROCESS.json',record)
    retain_evidence(ws,dest,stage)
    subprocess.run(['git','bundle','create',str(dest/'workspace.bundle'),'--all'],cwd=ws,check=True)
    print(model,system,stage,record['exit_code'],round(record['wall_seconds'],2),'inputs',record['protected_inputs_unchanged'],flush=True)

API_NOTE=('Your previous turn ended on a server-side API error (for example 529 Overloaded), not because of anything '
          'you did. Your working tree and evidence are intact. Continue the same task from where you were and '
          'finish with the report it asks for.')

def api_failed(dest):
    """True when the session's final result is a server-side API error."""
    events=[json.loads(l) for l in (dest/'events.jsonl').read_text().splitlines() if l.strip()]
    r=next((e for e in reversed(events) if e.get('type')=='result'),{})
    return bool(r.get('is_error')) and (r.get('api_error_status') or str(r.get('result','')).startswith('API Error'))

def retry_api(model,ws,dest,record,attempts=3,tools=None):
    """Resume a session that ended on an API error, keeping each failed attempt's files."""
    session=record['session_id'];history=record.get('api_errors',[])
    while api_failed(dest) and len(history)<attempts:
        n=len(history)+1
        for name in ('events.jsonl','stderr.log','REPORT.md'):
            if (dest/name).exists():shutil.copy2(dest/name,dest/f'api-error-{n}-{name}')
        history.append({'attempt':n,'ended_at':record.get('ended_at'),'result':(record.get('result') or {}),'files':f'api-error-{n}-*'})
        time.sleep(60*n)
        with quiet(False):record=run_native(model,ws,dest,API_NOTE,resume_session=session,**({'tools':tools} if tools else {}))
    if history:record['api_errors']=history
    return record

def resume_api(model,system,stage):
    """Resume an already-finished stage whose session ended on an API error; re-finish it."""
    ws=workspace(model,system);dest=OUT/f'{model}-{system}'/stage
    first=json.loads((dest/'PROCESS.json').read_text());assert api_failed(dest),'stage did not end on an API error'
    shutil.copy2(dest/'PROCESS.json',dest/'api-error-1-PROCESS.json')
    record=retry_api(model,ws,dest,dict(first))
    record['resumed_after_finish']={'first_commit':first['commit'],'note':'stage was committed and evaluated before the resume; downstream steps rerun'}
    finish(model,system,stage,record,first['base_commit'])

RESUME_NOTE=('The parent harness stopped your previous session process by mistake (a supervisor time limit), '
             'not because of anything you did. Your working tree and evidence are intact. Continue the same task '
             'from where you were and finish with the report it asks for.')

def resume(model,system,stage):
    """Resume a producer session the parent harness killed; record the interruption."""
    ws=workspace(model,system);dest=OUT/f'{model}-{system}'/stage
    first=json.loads((dest/'PROCESS.json').read_text());assert 'exit_code' not in first,'stage already completed'
    session=first['argv'][first['argv'].index('--session-id')+1]
    for name in ('events.jsonl','stderr.log','PROCESS.json'):shutil.copy2(dest/name,dest/f'interrupted-{name}')
    with quiet(False):record=run_native(model,ws,dest,RESUME_NOTE,resume_session=session)
    record['interruption']={'cause':'parent supervisor background time limit killed the session process',
        'first_attempt':{k:first.get(k) for k in ('argv','started_at','pid')},'resume_message':RESUME_NOTE,
        'first_events':'interrupted-events.jsonl'}
    finish(model,system,stage,record,json.loads((dest/'BASE.json').read_text())['base_commit'] if (dest/'BASE.json').exists() else git(ws,'rev-parse','HEAD'))

def run_native(model,ws,dest,packet,tools='Bash,Read,Write,Edit,Glob,Grep',resume_session=None):
    name,effort=MODELS[model];session=resume_session or str(uuid.uuid4())
    argv=['claude','-p','--safe-mode','--strict-mcp-config','--model',name,'--effort',effort,'--tools',tools,
          '--permission-mode','bypassPermissions','--output-format','stream-json','--verbose',*(['--resume',session] if resume_session else ['--session-id',session])]
    r=dict(argv=argv,cwd=str(ws),started_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),packet_sha256=sha(dest/'packet.txt'));start=time.monotonic()
    with (dest/'events.jsonl').open('w') as out,(dest/'stderr.log').open('w') as err:
        proc=subprocess.Popen(argv,cwd=ws,stdin=subprocess.PIPE,stdout=out,stderr=err,text=True);r['pid']=proc.pid;write(dest/'PROCESS.json',r);proc.communicate(packet)
    r.update(exit_code=proc.returncode,wall_seconds=time.monotonic()-start,ended_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
    events=[json.loads(l) for l in (dest/'events.jsonl').read_text().splitlines() if l.strip()]
    result=next((e for e in reversed(events) if e.get('type')=='result'),{});(dest/'REPORT.md').write_text(result.get('result',''))
    r['result']={k:result[k] for k in ('subtype','is_error','duration_ms','num_turns') if k in result}
    r['native_models']=sorted({e['message']['model'] for e in events if e.get('type')=='assistant' and 'model' in e.get('message',{})})
    r['session_id']=session
    matches=list((Path(os.environ['CLAUDE_CONFIG_DIR'])/'projects').rglob(session+'.jsonl'))
    if len(matches)==1:
        native=dest/matches[0].name;shutil.copy2(matches[0],native);r['native_sha256']=sha(native)
    else:r['native_capture_error']=str(matches)
    write(dest/'PROCESS.json',r);return r

if __name__=='__main__':
    if len(sys.argv)>=2 and sys.argv[1]=='prepare':print(prepare(*sys.argv[2:4]))
    elif len(sys.argv)>=2 and sys.argv[1]=='resume':resume(*sys.argv[2:5])
    elif len(sys.argv)>=2 and sys.argv[1]=='resume-api':resume_api(*sys.argv[2:5])
    elif len(sys.argv)>=2 and sys.argv[1]=='retain':retain_evidence(workspace(*sys.argv[2:4]),OUT/f'{sys.argv[2]}-{sys.argv[3]}'/'initial')
    else:launch(*sys.argv[1:])
