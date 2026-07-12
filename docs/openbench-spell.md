# OpenBench variantes — nota de diseño (S7): torre de control multi-proyecto

Base: fork `sscg13/OpenBench@shatranj` (clonado en `../openbench-spell`), que ya soporta
variantes, redes NNUE por SHA y SPSA distribuido. Decisión del propietario (2026-07-12):
**una única instancia OpenBench como torre de control para TODOS los proyectos de motores**
(Spell-Stockfish hoy; Atomic-Stockfish y sucesivos mañana), igual que el fork de sscg13
sirve a múltiples engines.

## Cómo lo hace el fork shatranj (estudiado)
- `Client/worker.py:379-391`: infiere la variante del NOMBRE del libro de aperturas
  (`'SHATRANJ' in book_name`) y pasa `-variant shatranj` a **cutechess-cli**. El resto
  (SPRT, lotes SPSA con `param['flip']`, redes por SHA, builds por make, registro de
  motores en `Engines/*.json`) es agnóstico al motor y a la variante.
- OpenBench es multi-engine NATIVO: cada motor es un json con repo/branch/make/bench.

## Diseño: ruteo variante → runner (generalización del truco del libro)

En vez del if de shatranj, una tabla en el worker:

    VARIANTS = {
        # token en el nombre del libro : (runner, args)
        "SHATRANJ": ("cutechess", "-variant shatranj"),
        "ATOMIC":   ("cutechess", "-variant atomic"),      # cutechess lo soporta nativo
        "FRC":      ("cutechess", "-variant fischerandom"),
        "SPELL":    ("uci-pair-runner", "spell-chess"),    # cutechess NO lo conoce
        # futuras variantes exóticas → uci-pair-runner con su id UCI_Variant
    }

- **Vía cutechess** (sin cambios del fork): toda variante que cutechess-cli conozca
  (atomic, shatranj, frc, horde, etc.). Atomic-Stockfish entra por aquí gratis.
- **Vía uci-pair-runner** (lo nuevo): pair-runner UCI puro para variantes que cutechess no
  arbitra. Generalización de `tools/fixed_nodes_match.py` a control de tiempo real, con
  salida stdout COMPATIBLE con cutechess ("Score of ...", "Finished game ...", "Elo
  difference ...") para que el parser del worker no cambie. El runner no necesita conocer
  reglas: los motores arbitran (bestmove (none), scores, adjudicación) — sirve para
  cualquier variante UCI futura.

## Piezas por proyecto (patrón repetible)
1. `Engines/<Motor>.json`: repo/branch/make/bench-signature. Spell-Stockfish primero;
   Atomic-Stockfish reutiliza el patrón tal cual cuando exista.
2. Libro(s) en `Books/` con el token de variante en el nombre
   (`SPELL_book.epd`, `ATOMIC_book.epd`...).
3. Redes por SHA-256 en el subsistema del fork (run5rl/run6+ para spell; las de atomic el
   día que toquen).
4. SPSA: los parámetros expuestos como opciones UCI (nuestro TUNE) — ya compatible.

## Orden de trabajo S7
1. Pair-runner UCI con TC real + salida compatible cutechess (extensión del driver actual).
2. Tabla de ruteo variante→runner en `Client/worker.py` (diff mínimo y upstream-able).
3. Despliegue local del servidor Django + `Engines/Spell-Stockfish.json` + libro SPELL.
4. Worker local end-to-end: SPRT de humo (mismo binario ≈ Elo 0) + SPSA de humo.
5. Workers remotos cuando haya más hardware; alta de Atomic-Stockfish cuando exista el repo.
