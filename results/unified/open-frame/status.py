"""One line per trajectory: last pipeline step, process failures so far, errors. Usage: python3 -B status.py WAVE"""
import json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
wave=sys.argv[1];logs=ROOT/'.build/open-frame'/wave/'logs'
pas=Path(__file__).resolve().parent/wave/'PASS.json'
done={k:v for k,v in (json.loads(pas.read_text())['trajectories'].items() if pas.exists() else []) if 'exit_code' in v}
for log in sorted(logs.glob('*.log')):
    lines=log.read_text().splitlines()
    steps=[l for l in lines if l.startswith(('step ','skip '))]
    last=f"done exit={done[log.stem]['exit_code']}" if log.stem in done else (steps[-1] if steps else '-')
    print(f"{log.stem:22} {last:18} fails={sum('FAIL' in l for l in lines):3d} errors={sum('Traceback' in l for l in lines)}")
