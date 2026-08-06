# Dónde están los cientos de Elo (revisión estratégica, 5-ago-2026)

Encargo del propietario: "todo se está alargando mucho (terreno neutral); revisar
qué significa vs toda la info que ya tenemos y pensar cómo seguir ganando
cientos de Elo (como hizo Opus con su parche)".

## 1. El diagnóstico: qué dice el terreno neutral

La foto de la cola (5-ago, ~35 SPRTs vivos): LLRs entre −1,9 y +1,6 tras miles
de partidas cada uno. Ni pases ni suspensos — **efectos verdaderos de ±0-2 Elo**,
que un SPRT [0,3] tarda eternidades en resolver. Esto no es mala suerte: es la
firma de un espacio agotado.

**Por qué está agotado**: spsa90 ya subió la colina de las constantes de poda.
Las rondas UB (semántica de capturas atómicas, programa ubdip) y SB (ronda 3
spell) re-parametrizan la MISMA familia de heurísticas alrededor del mismo
óptimo local. Son correcciones semánticamente justas (el proxy PieceValue ES
incorrecto en atomic) que valen +0-2 Elo cada una encima de un chasis ya
tuneado. Veinte de esas no son +40: son veinte SPRTs eternos.

**Dónde salieron los saltos grandes, con recibos**:

| Palanca | Elo | Coste | Clase |
|---|---|---|---|
| Disagreement mining (spell, 1 ronda) | **+82/+105** gates, +44/+62 vs legacy | 4 min de GPU + una tarde | datos/eval |
| Expansión táctica post-bestmove (parche de Opus) | **+55** | un parche estructural + destilación λ1.0 | forma de búsqueda |
| Campeonas de runs de entrenamiento (run5rl, run3b, HARD2) | decenas-cientos acumulados | días de GPU | datos/eval |
| Mejor knob de las rondas 2-3 actuales | +2-3 (T107/T119) | semanas de flota | constante |

La conclusión no es sutil: **los cientos de Elo han venido SIEMPRE de (a) qué
datos ve la red y (b) qué forma tiene el árbol — nunca de re-pesar podas**.
El propio math_god apunta igual: "meto una NNUE nueva en MVSF y destruye a
fairy y atomic-stockfish" — su apuesta es que la eval manda el techo. Y
nuestro listón spell parecía estar en −74 a −164 vs FSF+run5rl según TC.
**Corregido el 5-ago**: aquella medida cruzaba Spell-SF+HARD2 contra
FSF+run5rl. Con la misma red a los dos lados, Spell-SF gana +219/+200/+191
con LOS 100% a los tres TCs, y HARD2 es el eslabón flojo (pierde ~50-105
Elo contra run5rl dentro de nuestro motor). Ver `ubdip-spell-program.md`:
el "hueco grande de eval" contra el generalista no está medido; lo que sí
está medido es un hueco de eval **entre nuestras propias redes**.

## 2. El patrón del parche de Opus (el playbook, no la anécdota)

El +55 no fue un knob afortunado. Fue: **ceguera diagnosticada** (tácticas que
el árbol no miraba) → **cambio estructural que la ve** (expandir hijas tras el
bestmove) → **destilación que lo fija en la red** (λ1.0 obligatorio, juzgar a
3 TCs). Búsqueda y red co-evolucionando, no compitiendo por el mérito.

Tenemos EXACTAMENTE una ceguera de esa clase diagnosticada y localizada: la
brecha de mates 75-76/84 vs 83/84 de MV-SF-2018, que vive en exclusiva en los
mates largos (≥20 plies: 29/36 vs 35/36) por líneas QUIETAS que aterrizan en
el anillo del rey — y el intento 2 demostró con tres bugs de medición cazados
que los knobs encima del orden actual no la compran (el king-ring bonus murió
por sus propios recibos: los bestMove ya salen a rango 1,35). Lo que MV-SF
tiene distinto es ESTRUCTURAL: ~2× presupuesto de quiets por PENDIENTE
(AtomicMcpSlope), no por base. Ese es el candidato a "parche de Opus" de
atomic — una apuesta grande, no diez pequeñas.

## 3. El programa (orden de ejecución)

### A. Minería ronda 2 — la palanca más barata sin usar (spell primero)

El doc de mining lo deja escrito: la palanca satura ~2 rondas POR POOL de
partidas y se resetea con partidas nuevas. Desde la ronda 1 hemos jugado
cientos de miles de partidas nuevas de flota (rondas 2-3 = pool on-policy
fresco, gratis, ya en los match.log de OB). Nadie ha pasado la ronda 2.

- Spell: cosechar logs de rondas 2-3 → minar desacuerdos HARD2 vs FSF+run5rl
  (la referencia es MÁS fuerte: los desacuerdos apuntan a nuestro hueco) →
  relabel d4 → fine-tune λ1.0 → gates a 3 TCs. Expectativa honesta: decenas
  de Elo si el pool nuevo resetea la saturación; una tarde de trabajo.
- Atomic después, con árbitro = búsqueda profunda propia (query-by-committee
  run3b vs legacy run5rl como selector de posiciones).
- El bucle se auto-alimenta: cada campeona nueva juega sus propios matches.

### B. Datagen run9 → siguiente generación (ya en vuelo)

T89 (datagen-run9-hard2-50m) está generando el pool del próximo run. La
minería (A) se APILA encima de cada generación — son la misma palanca a dos
escalas de tiempo.

### C. La apuesta estructural: AtomicMcpSlope + co-evolución

Dossier #5 del intento 2, ascendido a apuesta principal: presupuesto de
quiets por pendiente (la diferencia estructural real con MV-SF-2018), juzgado
con los gates redefinidos (coste en d10-d17/84-FENs, subconjunto ≥20 plies,
ambos signos, contador de disparo). Si el árbol nuevo VE los mates largos, la
fase 2 es el playbook de Opus: destilar partidas/labels del buscador
arreglado a la red. Una apuesta grande bien medida > diez knobs neutros.

### D. Triaje de la cola (propuesta — decide el propietario)

Dejar de pagar flota por resolver ±0,5:

- **Matar ya** (LLR<−0,5 tras >3k partidas, sin tesis pendiente): T131
  (−1,93), T132 (−0,70), T105 (−0,69), T103 (−0,62), T161 (−0,57), T120
  (−0,52), T115 (−0,43), T137 (−0,41), y los MV-R menores en negativo
  persistente (T110/T111/T114/T116/T134). Post-mortem de una línea cada uno
  al dossier; el mandato de persistencia se cumple con la SEGUNDA
  parametrización dirigida, no manteniendo la primera en soporte vital.
- **Dejar correr**: T135 (+1,63, cerca), T152 (+0,60), T117 (+0,92), los
  LTC de MV-R4 (T142/T144/T150), T133, y T163/T159 (árbitros del mate-lab).
- **Subir el listón de entrada**: SPRT nuevo solo con recibo previo que
  prediga ≥3 Elo (fire-rate, subset de mates, bench) o siendo combo de
  supervivientes. Los knobs sueltos de <2 Elo esperados no entran.

### E. Referencias que suben el listón

MV-SF buenos del 13-sep y 7-nov-2019 (intel de ijhy; el de nov lo encontró
Gannet) — compilarlos sube la vara del mate-lab desde 83/84. Y la amenaza
math_god (MVSF+NNUE moderna) es también nuestra oportunidad: ese experimento
lo podemos correr nosotros primero contra nuestros propios motores.

## 4. Qué NO cambia

El programa ubdip no se archiva: U1 (blast_see con signo) sigue siendo la
fundación correcta y varios U-sites quedan sin probar con recibos. Cambia su
PRESUPUESTO: ideas nuevas de esa familia pagan el listón de entrada (D) y se
baten en combos, no de uno en uno contra la flota entera.

El orden A→B→C no es dogma de calendario: A empieza hoy (no gasta flota),
B ya corre, C se implementa mientras A gatea. D libera la máquina que A y C
van a necesitar.

## Revisión 6-ago (post-release 1.0): estado de cada frente

**El listón spell está RESUELTO y este doc queda corregido en su premisa**:
la medición formal del release (400/300/100 partidas fijas, misma red
run5rl a ambos lados, arnés con timeout y descartes contados) da
**+221/+271/+168 con LOS 100%×3 contra el baseline de referencia**. El
"−74..−164" del §1 era el cruce HARD2-vs-run5rl, no el chasis. La vara
nueva es interna: regla de coronación (toda campeona bate también a
run5rl) y auto-mejora. Los ~300 Elo del listón viejo quedan archivados
como incomparables (script del panel perdido; relay exonerado).

**Atomic — veredictos del 5/6-ago:**
- **R-A (orden)**: negativo en flota (T166 −1.3 muriendo, T167 −0.7) y
  POR DEBAJO de la base pura en la suite de mates bajo condiciones
  idénticas (78 vs 80). El reorden solo no paga. Post-mortems al morir;
  el fix del bug de EP de la BASE (movepick.cpp:215) sobrevive como
  candidata mínima propia.
- **R-B**: familia B refutada; C2-solo refutada (es UN knob; −2 mates
  largos por −17% de árbol). `AtomicSeeDisc` sin consumidor → se
  re-cablea a futility/statScore DENTRO de la apuesta viva. **La apuesta
  viva de atomic es AtomicMcpSlope** (§C de este doc): presupuesto de
  quiets por PENDIENTE, la única diferencia estructural medida con
  MV-SF-2018 — y sigue SIN testear. Es el siguiente diseño grande.
- **R-C**: fundación probada bit-exacta (0 discrepancias en 2.270M
  comprobaciones); viaja dentro del paquete que la consuma.
- **R-E**: Gate 1 GANADO (vena +33,5 real vs control, fuga descartada);
  Gate 2 = empate de Elo a 3 TCs → cambio de eje: las etiquetas-teorema
  van a la MEZCLA de datos de la próxima red atomic, no a fine-tune. Las
  suites-oráculo quedan como infraestructura de recibos.
- Supervivientes de cola: T135 (2.50, a nada del pase → clon LTC +
  auditoría por máquina), T152/T117/T133/MV-R4-LTC siguen su curso.

**Spell — veredictos del 5/6-ago:**
- **S-A es EL frente**, con el recibo definitivo de la fase 1 de S-D: el
  tope 8F/4J esconde mates reales (pos84 canta mate 4 con gates32/20) y
  poda defensas del rival en nodos sin jaque. T170 (SPSA del picker sobre
  run5rl, config idéntica, knobs reseteados) corriendo; a su convergencia
  → SPRT mínimo con pos42/pos84 de recibos antes/después.
- **S-D**: refutado como fix de mates (la exención urgente EMPEORA
  pos84); su orden tras S-A/S-B queda reforzado con recibos. Fire-rate
  del estado urgente (19,4%) archivado para el rework de qsearch.
- **S-B/S-C**: sin cambios — S-B espera las clases de S-A; SB9 (+0.60)
  decide el embrión de S-C.
- **S-E (dominante)**: flota entera sobre run5rl; T169 genera el pool
  run9 con seed independiente verificado contra 18 datagens históricos;
  **run1c se diseña AHORA** (receta antes de que el pool llene). Minería
  saturada hasta que el pool nuevo resetee la palanca (tesis del doc de
  mining, confirmada por r3).

**Transversal**: el desierto de knobs neutros SIGUE (toda la cola en
±1 tras miles de partidas) — refuerza la tesis de este doc: apuestas
grandes (slope en atomic, picker en spell) y datos (run1c, mezcla R-E),
no trocitos. Deuda infra taskeada: portada bajo ráfaga (#18, recibos de
la sonda), orden de locks del parent-wake, verificación del cierre
guard×8.
