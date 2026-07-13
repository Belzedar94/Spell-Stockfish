# Spell-Stockfish — Plan maestro v2

Objetivo: **toda la estructura y la base para el mejor motor de Spell Chess del mundo**,
con el ciclo completo motor ↔ generador ↔ trainer ↔ testing distribuido. Superar a la
baseline en Elo deja de ser un hito bloqueante: es la métrica continua que optimizará el
molino de ideas sobre OpenBench. (Decisión del propietario, 2026-07-12; estructura
adaptada del plan Atomic-Stockfish.)

Fuentes de verdad:
- Oráculo inmutable: `FSF_Spell_test_baseline.exe` (Belzedar94/Fairy-Stockfish @8868ab43,
  spell/nnue-potions) + `spell-chess_run5rl_e10_l07.nnue`.
- Contrato de comportamiento: `SPELL_SPEC.md`. Registro de decisiones/iteraciones: `AUDIT.md`.
- Upstream Stockfish fijado (@9a8dd81d) hasta completar la serie 1.x.

Decisiones de alcance (propietario):
- **Interfaces v1 = superficie completa**: UCI + XBoard/CECP + Python (pyffish-spell) +
  JavaScript CommonJS/ESM + WASM (ligero y NNUE).
- **Datos**: formato legacy 76 bytes (verificado contra el oráculo) para la serie run6;
  `spell-bin-v2` (magic/versión/schema-hash + manifiesto JSON) llega con el trainer moderno.
- **Testing distribuido**: OpenBench multi-máquina, partiendo del fork con soporte de
  variantes+NNUE+SPSA: https://github.com/sscg13/OpenBench/tree/shatranj

## Estado por hitos (equivalencia con el plan Atomic)

| Hito | Contenido | Estado |
|---|---|---|
| S0 | Baseline congelado, spec, suite perft (61 pos), harness | ✅ |
| S1 | Reglas completas sobre SF master (perft 61/61, FEN byte-idéntico, 22 tests, KING nativo, constantes de compilación) | ✅ |
| S2 | NNUE legacy `0x7AF32F20` (paridad exacta, incremental+Finny, export byte-idéntico) | ✅ |
| S3 | Búsqueda fuerte: de -568 histórico a **VSTC -146 / STC -111 / LTC -55** (panel formal 2026-07-13, binario sano + SPSA-2, ambos run5rl). Continúa como métrica del molino S8 | 🔶 continuo vía OpenBench |
| S4 | Pipeline end-to-end ✅ CERRADO (2026-07-12): 1,9M posiciones propias → barrido λ {0.25/0.75/1.0} ×4 épocas → bracket (λ1.0 gana monótono) → **λ1.0 vs sin-red: VSTC +179 LOS 100%** — pipeline validado por el propietario. Red: spell_run6a_l10.nnue. Dataset serio (loop RL 100M/iter) = trabajo futuro | ✅ |
| S5 | Suite obligatoria ✅ CERRADO (2026-07-13): units (24) + protocolo UCI + repro + signature + XBoard + XBoard hostil + perft vs oráculo, orquestador run_suite (quick/full), **CI verde en GitHub Actions** (build+bench+firma y job de sanitizers ASan/UBSan con units+perft+bench d5; el assert de evaluate-en-jaque cazado por ASan se retiró con racional — spell evalúa en jaque por diseño) | ✅ |
| S6 | Protocolos y bindings: XBoard/CECP ✅ endurecido (ronda adversarial: UAF en quit, deadlock analyze, '.', atoi; 6 escenarios hostiles en suite); pendiente: API compartida (notation.{h,cpp}), wheel pyffish-spell, ffish.js CJS+ESM, WASM Board + WASM UCI/NNUE, paridad cross-surface — plan en docs/bindings-port-plan.md | 🔶 |
| S7 | OpenBench ✅ CERRADO (2026-07-13): torre operativa multi-proyecto — servidor público (repo Belzedar94/OpenBench@spell-runner), web vía túnel cloudflare, worker 24T local, ruteo SPELL→uci_pair_runner endurecido, SPSA (2 sesiones, 19.2k partidas) y SPRT a escala (cola de 10+ tests), redes/books por SHA, presets metodología sscg13. Docs: openbench-spell.md, openbench-server-runbook.md, AGENTS.md del fork (guía multi-agente) | ✅ |
| S8 | Molino de ideas EN MARCHA: metodología fishtest/sscg13 (SPRT STC→LTC→merge), 10 toggles branching-1650 en cola (#16-24), tabla de refutación de spells (#25) y bono freeze-jaqueadores (#26, consejo ubdip); SPSA-2 aplicada como defaults; refutados: merged-ordering (LLR -2.01), razor-guard (-1.01 parado), C9 quiet-min-depth, redundant-freeze (rules-dead) | 🔶 |

Orden propuesto: S4 → S5 → S7 → S6 → S8 (OpenBench antes que bindings para desbloquear el
molino de ideas cuanto antes; ajustable).

## Gate por hito mayor
1. Build x86-64-bmi2 release + debug/asserts (Windows MinGW; Linux GCC cuando haya CI).
2. Batería completa: perft 61/61 d2 vs oráculo, unit tests, eval-parity exit 0, bench firmado en BENCH_LOG.
3. Pipeline: datagen → psv_decode → loader → training step → serialize → carga en motor.
4. Desde S5: protocol/reprosearch/signature/instrumented aplicables.
5. Cambios de fuerza: **SPRT en la torre** (STC 8.0+0.08 → LTC 40.0+0.4, bounds
   [0.00, 5.00], win adj 4/800) — metodología sscg13/fishtest adoptada 2026-07-13;
   el panel local de 3 TCs queda retirado para iteración (solo progression tests
   vs release anterior + baseline FSF, presets progtest).

## Releases
- 0.1: reglas + unit tests completos — **S5 cerrado ⇒ procede etiquetar ya** (sin tag aún).
- 0.2: XBoard + Python + JS + WASM completos (S6).
- 0.5: pipeline NNUE completo con red propia entrenada (run6+) y búsqueda no inferior a la actual.
- 1.0: suite completa + OpenBench operativo + mejora demostrada sobre baseline en los 3 TCs.

## Intel de Discord (Fairy-Stockfish, escaneado 2026-07-12 vía bot fairy-vault)

- **S7/OpenBench — diseño ya validado por la comunidad** (hilo #development 2026-01-16/17):
  ubdip: el servidor mantiene la lista de tareas y los workers (procesos standalone) hacen
  polling — nunca push del servidor; el propietario concluyó y ubdip confirmó que el cambio
  clave es **sustituir cutechess por variantfishtest en el lado worker**. sscg13: OpenBench
  funciona para variantes soportadas por cutechess, lo demás requiere ese swap. Referencia:
  su fork sscg13/OpenBench@shatranj (variantes+NNUE+SPSA).
- **Trainer moderno**: sscg13 tiene `bullet` (el trainer estándar actual) soportando la
  arquitectura SF oficial y "probablemente adaptable" a variantes tipo-ajedrez (rey + 5
  tipos en 8x8). Candidato para la era spell-bin-v2/NNUE-v2 (S8) en vez del pytorch legacy.
- **Candidatos S8 del brainstorm de heurísticas (2025-09-29)**: (a) endurecer el filtro de
  jumps (solo si la jugada habilitada captura/da jaque/escapa) — AÚN SIN TESTEAR;
  (b) "skip redundant freezes" — REFUTADO 2026-07-13: rules-dead, la zona propia nunca
  está activa en el turno propio. Ya cubiertos: bonus rey/anillo, filtro de inutilidad,
  history por gate (retirado por SPSA-2: pesos→0).
- **Historia**: el bug de config chess-mode del trainer (PIECE_TYPES 6/POCKETS false → datos
  sin potions) que corregimos en F6 es exactamente el que el propietario sufrió el
  2025-09-30 durante run5. El crash por MAX_MOVES=4096 reportado por moky (2025-10-01)
  es la misma clase que nuestro fix 8192→32768+arena. ubdip modernizó variant-nnue-tools
  (2025-10-31: docs, convert_epd, puzzle gen) — útil para S5.
- **Comunidad**: pregunta sin responder en #general (2026-05-27, jasper_0011): "did you
  guys ever make an engine for evaluating spell chess positions?" — momento de anuncio
  natural para el propietario cuando el proyecto esté presentable.

## Notas operativas
- CI de GitHub ✅ verde en master (repo público desde 2026-07-12; sanitizers incluidos).
- Rotar el token del bot de Discord al cierre del proyecto (se pegó en chat).
- Redes y datasets se referencian por SHA-256.
- Anuncio público hecho en el Discord de FSF (2026-07-13); ubdip aporta ideas de search
  (registradas en AUDIT y convertidas en SPRTs #25/#26).
- Syzygy/tablebases: N/A para spell por diseño (reyes capturables + spells en mano no
  tienen TB definibles) — exclusión justificada, sin equivalente del hito Syzygy de Atomic.
