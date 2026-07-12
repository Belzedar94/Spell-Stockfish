# OpenBench-spell — nota de diseño (S7)

Base: fork `sscg13/OpenBench@shatranj` (clonado en `../openbench-spell`), que ya soporta
variantes, redes NNUE por SHA y SPSA distribuido. Diseño del worker validado en el Discord de
Fairy-Stockfish (2026-01-16/17): servidor con cola de tareas + workers standalone que hacen
polling; el único cambio estructural es el runner de partidas del worker.

## Cómo lo hace el fork shatranj (estudiado)
- `Client/worker.py:379-391`: infiere la variante del NOMBRE del libro de aperturas
  (`'SHATRANJ' in book_name`) y pasa `-variant shatranj` a **cutechess-cli**, que conoce esa
  variante nativamente. El resto (SPRT, lotes SPSA con `param['flip']`, redes, builds por
  make) es agnóstico a la variante.
- Motores registrados vía `Engines/<nombre>.json` (repo, rama, comando make, protocolo bench
  para validar binarios por firma — nuestro `bench` con firma en BENCH_LOG encaja directo).
- Libros en `Books/` con manifiesto.

## Adaptación para spell-chess (cutechess NO conoce la variante)
1. **Runner propio en el worker**: sustituir la invocación cutechess por un pair-runner UCI
   puro (generalización de `tools/fixed_nodes_match.py` a control de tiempo real, que ya
   maneja FENs spell, adjudicación por score y terminales spell). Dos variantes de
   integración:
   a. Emitir stdout compatible con cutechess ("Score of ... ", "Finished game ...", "Elo
      difference ...") para no tocar el parser del worker — **diff mínimo del fork** (opción
      preferida).
   b. Modificar el parser del worker (más limpio a largo plazo, más diff).
2. **Detección**: replicar el truco del libro — `'SPELL'` en el nombre del libro →
   runner spell. Libro inicial: `books/spell-chess.epd` del Match script.
3. **Engine config**: `Engines/Spell-Stockfish.json` con nuestro repo/branch, make
   x86-64-bmi2, y validación por firma de bench.
4. **Redes**: subsistema del fork tal cual (upload por SHA, fetch de workers). run5rl y
   run6+ por SHA-256.
5. **SPSA**: el fork ya distribuye lotes SPSA; nuestros 11 parámetros TUNE ya son opciones
   UCI → compatible sin cambios de motor.
6. **Servidor**: Django (requirements.txt del fork); despliegue inicial en el 5950X, workers
   locales primero, remotos después (el protocolo ya es multi-máquina).

## Orden de trabajo S7
1. Pair-runner con TC real + salida compatible cutechess (extensión del driver actual).
2. Despliegue local del servidor + registro del engine + libro spell.
3. Worker local end-to-end con un SPRT de humo (motor vs motor mismo binario ≈ Elo 0).
4. SPSA distribuido de humo.
5. Workers remotos (cuando haya más hardware).
