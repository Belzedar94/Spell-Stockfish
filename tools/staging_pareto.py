#!/usr/bin/env python3
"""Staging Pareto: recall/class-size de criterios AND sobre datos reales.

Fase 1 del programa (docs/spell-staging-program.md): lee run7 (run6a),
consulta `spellfeatures` del motor spell-lab por posicion, y evalua la
matriz de criterios offline. Positivos = registros cuyo move es un cast.

Uso:
  python tools/staging_pareto.py --engine src/stockfish.exe \
      --data .scratch/run6a-1m.run7 --pos 30000 --out .scratch/pareto.csv
"""

import argparse, subprocess, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'spellnnue-pytorch'))
import run7  # noqa: E402

COLS = ("mv sp gate royal ourK ourKJ ekAtt ekJmp silv matz attz gain logit v1").split()


def engine_features(proc, fen):
    proc.stdin.write(f"position fen {fen}\nspellfeatures\n")
    proc.stdin.flush()
    rows = []
    while True:
        line = proc.stdout.readline()
        if not line or line.strip() == "done":
            break
        parts = line.strip().split(';')
        if len(parts) == len(COLS):
            rows.append(dict(zip(COLS, parts)))
    return rows


def criteria(row):
    """Cada criterio devuelve True si el spell es 'early' bajo esa variante."""
    F = row['sp'] == 'F'
    royal, silv = row['royal'] == '1', int(row['silv'])
    ekAtt, ekJmp = row['ekAtt'] == '1', row['ekJmp'] == '1'
    matz, attz, gain = int(row['matz']), int(row['attz']), int(row['gain'])
    logit = int(row['logit'])
    v1 = row['v1'] == '1'
    return {
        'v1':       v1 or (not F and gain >= 800),
        'C1':       (royal and (ekAtt or ekJmp)) or (F and silv > 0) or (F and matz >= 1300)
                    or (not F and gain >= 800),
        'C2':       (royal and (ekAtt or ekJmp)) or (F and silv >= 300) or (F and attz >= 500)
                    or (not F and gain >= 800),
        'C3':       (royal and (ekAtt or ekJmp)) or (F and silv >= 300) or (F and attz >= 900)
                    or (not F and gain >= 1200),
        'C5_0':     logit >= 0,
        'C5_500':   logit >= 500,
        'C5_800':   logit >= 800,
        'C1+C5':    ((royal and (ekAtt or ekJmp)) or (F and silv > 0) or logit >= 500)
                    or (not F and gain >= 800),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--pos', type=int, default=30000)
    ap.add_argument('--out', default='.scratch/pareto.csv')
    a = ap.parse_args()

    proc = subprocess.Popen([a.engine], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, bufsize=1)
    names = list(criteria({k: '0' for k in COLS} | {'sp': 'F'}).keys())
    hit = {n: 0 for n in names}          # recall: PV-cast clasificado early
    early = {n: 0 for n in names}        # class-size: pares early
    npos = ncast = ntot = 0

    for rec in run7.iter_records(a.data):
        mv = run7.move_to_uci(rec.move) if hasattr(run7, 'move_to_uci') else None
        is_cast = bool(mv) and '@' in mv
        if not is_cast and npos % 7:     # muestrea no-cast 1/7 para class-size
            npos += 1
            continue
        npos += 1
        rows = engine_features(proc, run7.to_fen(rec))
        if not rows:
            continue
        for r in rows:
            ntot += 1
            for n, v in criteria(r).items():
                early[n] += v
        if is_cast:
            pv = next((r for r in rows if r['mv'] == mv), None)
            if pv:
                ncast += 1
                for n, v in criteria(pv).items():
                    hit[n] += v
        if npos >= a.pos:
            break

    proc.stdin.write("quit\n"); proc.stdin.flush()
    with open(a.out, 'w') as f:
        f.write("criterio,recall_pct,class_size_pct,pv_casts,pares\n")
        for n in names:
            f.write(f"{n},{100*hit[n]/max(ncast,1):.2f},{100*early[n]/max(ntot,1):.2f},"
                    f"{ncast},{ntot}\n")
    print(open(a.out).read())


if __name__ == '__main__':
    main()
