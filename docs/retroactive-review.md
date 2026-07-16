# Revisión retroactiva bajo el feedback de ubdip (2026-07-16)

Lente aplicada (ubdip 2026-07-14): (1) ¿se exploraron los ANDs de las
condiciones o se probó un punto del espacio?; (2) ¿el clasificador tenía miedo
a falsos negativos (ORs anchos)?; (3) ¿una idea compleja recibió más de UN
intento?; (4) persistencia calibrada: ni infinita ni one-shot.

Regla de clasificación: un TOGGLE atómico (on/off sin estructura interna) no
tiene espacio de diseño — un punto ES su espacio; el one-shot es correcto ahí.
La crítica aplica a ideas con criterios, umbrales, dosis o colocación.

## A. Cerradas correctamente (toggle atómico + señal clara) — no reabrir
conthist-skip, no-iir, lmp-200, aspiration, nullmove-guard, razor-guard,
gatehist-off, volatility-scale. Los picos de gatehist (+1.01) y volatility
(+1.26) que se desinflaron son paseo aleatorio clásico, no señal abandonada.

## B. Ya reabiertas con el método nuevo (en vuelo)
| Familia | Acción tomada | Test |
|---|---|---|
| futility-150 | re-run LTC directo (mecanismo pro-profundidad) | #57 |
| spell-refutation | re-run LTC directo (ordering compone con depth) | #58 |
| freeze-dedup | STC PASS → LTC rebasada | #62 |
| capture-see-120 | STC PASS → LTC (candidata a 1er merge) | #54 |
| jump-checkers | confundido (2 knobs juntos) → DESCOMPUESTO | #64/#65 |

## C. Cerradas PREMATURAMENTE — reabrir con matriz y presupuesto

### C1. Familia staging (la tesis de ubdip) — PRIORIDAD 1
v1 = un punto del espacio (clasificador OR + una colocación) con −77 en 896
partidas. El programa completo está en spell-staging-program.md: arnés offline
recall/class-size sobre PSV (sin coste Elo) → matriz de endurecimientos AND
(C1: freeze-al-rey exige jaque presente o jump-checker — el ejemplo literal de
ubdip; C2-C4; C5: logit del policy head como frontera) → Pareto → solo los 2-3
puntos Pareto van a SPRT, una variable por vez. Presupuesto: 6 SPRTs.
Prerequisito: arnés offline (tarea Codex, sin granja).

### C2. Policy head — no reabrir standalone; vive como C5 del staging
v2 falló como BONUS de ordering a UN peso (4096). Su uso cualitativamente
distinto (frontera de partición con umbral barrible) está sin probar y es
exactamente el eje C5 del programa staging. Cerrarlo como bonus, reabrirlo
como frontera.

### C3. Pilar C (qsearch consciente de spells) — reabrir tras staging
Una sola config probada (−1.10). El mecanismo (errores de horizonte por casts
quiet) es real — los mates por secuencia de cast existen en las partidas. El
AND sin explorar: QUÉ casts entran en qsearch (solo los que dan jaque o
habilitan captura de rey — el análogo exacto de "checks en qsearch" — versus
mi clase ancha original). Comparte ingredientes con el clasificador del
staging: secuenciar DESPUÉS. Presupuesto: 3 SPRTs.

### C4. jump-see-cost — completar la dosis-respuesta (barato)
Dosis 100 = fail claro; dosis 40 = +0.33 con solo 1.302 partidas (parada
manual, no veredicto). UN test options-only lo completa. Si el dueño no quiere
re-encolarlo, cerrar documentando "2 puntos, tendencia negativa a dosis alta,
inconcluso a baja".

### C5. no-penalty-pv — el sign-flip merece su experimento inverso algún día
+30 STC / −27 LTC es el resultado más cargado de teoría del ledger: las
penalizaciones de spell importan MÁS con profundidad. El experimento inverso
(penalización EXTRA solo a LTC) nunca se probó. Prioridad baja; 1 SPRT LTC
cuando la cola respire.

## D. Cerradas con presupuesto GASTADO — mantener cerradas
- **Pilar B (descomposición del cast)**: 2 intentos serios, −110 ambos.
  Reabrir SOLO si aparece ingrediente nuevo (p.ej. policy priors en el nodo
  pendiente). Condición explícita, no puerta abierta.
- **Pilar A (presupuesto por depth)**: subsumido por staging ("buscar menos
  spells pero mejor elegidos" ES la frontera del staging). Cerrado por
  superseded, no por evidencia propia.
- **MCTS policy-less**: −600 era el resultado esperado sin policy. El
  experimento REAL (PUCT + priors) queda parcado bajo la condición de salida
  ya pactada (gap LTC no baja de −55 tras el programa actual → spike A0).

## E. Cambios de proceso (ya operativos)
1. Toda idea con clasificador/umbral/dosis: **arnés offline antes que granja**.
2. Presupuesto de persistencia DECLARADO al abrir familia (toggle=1;
   compleja=6-10) y respetado en ambas direcciones.
3. Confundidos prohibidos: un knob por SPRT salvo interacción hipotetizada.
4. El ledger de AUDIT gana columna: "espacio explorado / presupuesto usado".
5. STC no filtra para LTC en spell (el sign-flip lo probó): las familias
   pro-profundidad van a LTC directo.

## Orden de ejecución propuesto
1. Arnés offline staging (Codex, ya) → Paretos → SPRTs C1.
2. Completar jump-see-cost=40 (1 test barato, si el dueño OK).
3. Tras staging: pilar C con clase estrecha (jaque/rey solo).
4. no-penalty-pv inverso a LTC (oportunista).
5. net-1 de SSNNv2 sigue su pipeline en paralelo (datagen → gate → train).
