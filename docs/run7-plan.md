# run7 — plan de generación de datos para SSNNv2 net-1 (2026-07-15)

Fuentes: receta histórica del propietario (Discord 2026-01-12), semántica de
filtros de ubdip (2025-09-19), fixed-nodes de sscg13 (2026-01-06), torneo de
redes de rainrat (2026-07-13: run4rl λ0.85 > run5rl λ0.7), aviso de
distribución de ceongceongi (85% tablas sin señal — nuestro riesgo es el
inverso), y el riesgo "90% manos llenas" del AUDIT de P1.

## Qué genera
- Motor: Spell-Stockfish master (reglas corregidas post-936ab213), config
  default (la misma que juega los SPRT — el motor más fuerte disponible).
- Comando in-engine `datagen` (implementación P2): self-play que escribe
  registros run7 (44 B) directamente. run8+ serán las iteraciones RL con net-1.

## Parámetros (v1 propuesta)
| Parámetro | Valor | Procedencia |
|---|---|---|
| Búsqueda | **fixed nodes 10000** (bootstrap nocturno; d4-5 medido) | decisión propietario 2026-07-15; calibración: 10k=d4-5, 20k=d5-6, 40k=d6-7; run8-RL regenerará con net-1 mejor y más rápido |
| count | 20M posiciones (bootstrap) | decisión propietario; ~15 h a 24 hilos con 10k nodos |
| Aperturas | spell_openings.epd (sin seeding sintético — ver medición) | libro legacy |
| random_move_count / min_ply / max_ply | 8 / 1 / 20 | receta propietario (los casts entran de forma natural) |
| random_multi_pv / diff | 4 / 100 | receta propietario |
| write_min_ply | 5 | receta propietario |
| eval_limit | 10000 | default probado del propietario (legacy run5) |
| Filtros al escribir | filter_checks 1; filter_captures 1 (extendido a capturas gated); filter_promotions 0; eval_diff_limit 32000 (deshabilitado) | flags exactos del legacy run5 del propietario |
| Adjudicación | ninguna (resultado real; partidas cortas) | 3% tablas — sin necesidad de libros anti-tablas |
| Terminales | incluir pre-captura-de-rey etiquetadas mate | semántica del registro P1 |
| Dedup | no en v1 (la aleatorización basta) | estándar |

## Sin seeding sintético: la medición lo mató (2026-07-15)

Histograma sobre 1M de posiciones de partidas reales (run6a): fase 0 = 57,5%,
fase 1 = 31,3%, fase 2 = 11,2% — las partidas queman pociones rápido y juegan
largo sin ellas; freeze viva 12,1%, jump vivo 2,3%. La cobertura de fases
tardías es natural y abundante; las manos llenas entran solas por los primeros
plies de cada partida (write_min_ply 5). Posiciones sintéticas habrían metido
combos (estructura, manos) que el juego real no visita — riesgo de distribución
sin necesidad (objeción del propietario, confirmada por los datos).

## Gate de auditoría ANTES de entrenar (P2 no termina sin esto)
Informe de distribución sobre los 50M: WDL (esperado ≈ 50/47/3), histograma de
fase-poción (**ningún bucket de fase < 5%**), buckets de material, histogramas
de manos/cooldowns, evals, plies, y % de posiciones con zona VIVA (objetivo
≥3% freeze viva y ≥1,5% jump vivo — si no, subir seeding o write-rate cerca de
casts). Si el informe suspende, se ajusta y regenera ANTES de gastar GPU.

## Entrenamiento net-1 (tras el gate)
- λ ∈ {0.75, 0.85} en runs cortos (la evidencia de rainrat favorece 0.85);
  decide val-loss + mini-gauntlet, luego run largo.
- Factorización activada (gates freeze + frozen→pieza, ya en el trainer P1).
- Gauntlet: SPRT STC [1,6] vs default actual; si pasa → LTC → panel formal.

## Logística
- Generación nocturna: parar worker de OpenBench, 24 hilos de datagen,
  relanzar worker al alba (o cuota 12/12 si se quiere granja en paralelo).
- Almacenamiento: 50M × 44 B ≈ 2,2 GB (+ índice). En .scratch/, con receta
  y checksum en tools/spellnnue-pytorch/ como sample-network.json.
