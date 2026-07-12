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
| S3 | Búsqueda fuerte (de -568 a LTC -105 / nodos-fijos -17 con red idéntica) | 🔶 pausado; se retoma vía OpenBench |
| S4 | Pipeline end-to-end: datagen nativo ✅ formato-verificado, granja ✅, loader ✅, paso GPU ✅, serializador byte-idéntico ✅ → **run6 real: generar datos + entrenar + cargar + jugar** | ⏭ EN CURSO |
| S5 | Suite obligatoria: spell_tests ampliado (release+debug), perft.sh, protocol.sh (UCI+XBoard), reprosearch.sh, signature.sh (corpus spell), instrumented (ASan/UBSan), pipeline-CI; sin skips nuevos sin justificar | ⏳ |
| S6 | Protocolos y bindings: XBoard/CECP; API compartida; wheel pyffish-spell; ffish.js CJS+ESM; WASM ligero y NNUE; tests de paridad native/Python/JS/WASM | ⏳ |
| S7 | OpenBench-spell: servidor + workers multi-máquina, SPRT, SPSA, gestión de redes; adaptación del fork shatranj | ⏳ |
| S8 | Molino de ideas: búsqueda (candidatos en AUDIT: dominancia del base, descomposición aditiva del producto base×gate, forense de derrotas) y redes (arquitecturas, datos, distill) con gates SPRT | ⏳ |

Orden propuesto: S4 → S5 → S7 → S6 → S8 (OpenBench antes que bindings para desbloquear el
molino de ideas cuanto antes; ajustable).

## Gate por hito mayor
1. Build x86-64-bmi2 release + debug/asserts (Windows MinGW; Linux GCC cuando haya CI).
2. Batería completa: perft 61/61 d2 vs oráculo, unit tests, eval-parity exit 0, bench firmado en BENCH_LOG.
3. Pipeline: datagen → psv_decode → loader → training step → serialize → carga en motor.
4. Desde S5: protocol/reprosearch/signature/instrumented aplicables.
5. Cambios de fuerza (cuando aplique): panel 3 TCs (2000+20 / 10000+100 / 30000+300,
   LOS 100%, >100 partidas) — vía OpenBench SPRT cuando S7 esté operativo.

## Releases
- 0.1: reglas + unit tests completos (≈ ya cumplido; etiquetar tras S5-parcial).
- 0.2: XBoard + Python + JS + WASM completos (S6).
- 0.5: pipeline NNUE completo con red propia entrenada (run6+) y búsqueda no inferior a la actual.
- 1.0: suite completa + OpenBench operativo + mejora demostrada sobre baseline en los 3 TCs.

## Notas operativas
- CI de GitHub bloqueado por billing de la cuenta (decisión pendiente del propietario);
  mientras tanto, todos los gates corren en local.
- Rotar el token del bot de Discord al cierre del proyecto (se pegó en chat).
- Redes y datasets se referencian por SHA-256.
