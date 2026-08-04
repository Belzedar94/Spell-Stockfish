# Programa UB: heurísticas de búsqueda atómicas (feedback de ubdip, 30-jul-2026)

Origen: revisión de código de ubdip (lead de FSF) sobre Atomic-Stockfish.
Su diagnóstico: el fork conserva las heurísticas de captura del ajedrez
estándar, que en atomic son semánticamente incorrectas (capturar NO gana la
pieza capturada: explota también el capturador y el anillo). Inventario
verificado por nosotros el 30-jul (rutas sobre `fable/mv1-futility`):

## Inventario (recibos)

| Sitio | Estado | Item ubdip |
|---|---|---|
| `position.cpp:1382 see_ge` | YA blast-aware (anillo, reyes, quiets con captura explosiva, `--result` de tempo token) | base |
| `position.cpp:1387` | promos/EP salen por `type_of() != NORMAL` → "simple SEE" | 4 |
| `search.cpp:1486` | futility de capturas suma `PieceValue[capturedPiece]` | 1 |
| `search.cpp:2090` | qsearch futility suma `PieceValue[pieza en to_sq]` | 1 |
| `search.cpp:1641` | `statScore = 809*PieceValue[captured]/128 + captHist` | 10 |
| `movepick.cpp:215` | orden de capturas = `captHist + 7*PieceValue[captured]` | 2 |
| `movepick.cpp:236` | penalización "threatened by lesser" con PieceValue | 2 |
| `captureHistory[pc][to][capturedType]` | sin datos de anillo | 3 |
| `types.h:226 AtomicCapturePieceValue` | tabla propia ya existe | — |
| `AtomicFutilityScale/QsFutilityBase/AtomicCaptFut*` | constantes atómicas TUNEd ya existen | 11 |

Clave estratégica: **la primitiva correcta (delta de explosión) ya existe
dentro de `see_ge`, pero es booleana-por-umbral; los consumidores usan el
proxy ruidoso.** El programa extrae la primitiva y la cablea sitio a sitio.

## Escalera de parches (independientes, cada uno su rama y su SPRT)

- **U1 `blast_see(Move) -> Value`** (fundación, sin cambio de juego):
  refactor del bucle de `see_ge` a una función que devuelve el delta con
  signo; `see_ge` pasa a compararla con el umbral (mismo resultado bit a
  bit → bench idéntico). Extensión item 4: promos capturadoras (delta +
  valor de promoción − peón), EP (espejo EXACTO de la semántica de
  `do_move` atómico). VALIDACIÓN: test diferencial `blast_see` vs
  contabilidad material real de `do_move`/`undo_move` sobre millones de
  posiciones de perft/bench; perft intacto; bench idéntico al padre si
  see_ge no cambia de veredicto en NORMAL.
- **U2 futility por blast**: `search.cpp:1486` y `search.cpp:2090`
  sustituyen `PieceValue[captured]` por `blast_see(move)` (el `see_ge` de
  2102 se queda). Constantes existentes se conservan; factor nuevo TUNE.
- **U3 orden de capturas por blast**: `movepick.cpp:215` →
  `captHist + K*blast_see(move)` (K TUNE, arranque 7 equivalente).
- **U4 statScore por blast**: `search.cpp:1641` → `809*blast_see/128`
  con clamp inferior (blast negativo = malus real, no underflow).
- **U5 history con anillo** (item 3): añadir dimensión
  `min(popcount(anillo explosivo sin peones), 7)` a captureHistory
  (8 buckets, ~8× memoria como estima ubdip). Sitios: lectura movepick
  215/search 1478/1642, escritura en update_capture_stats (~1876).
- **U6 malus de tempo SEE≈0** (item 9, la idea más profunda): en atomic
  una captura de intercambio igualado PIERDE tempo (no hay recaptura).
  `AtomicCaptureTempo` TUNE (~20-40cp) restado en el ORDEN (U3) primero;
  variante 2: también en umbrales de poda. ubdip no lo logró en MV-SF —
  esperamos que el chasis moderno + SPSA lo rescaten.
- **U7 amenazas sin sentido atómico** (item 2b): primera versión =
  TEST DE ELIMINACIÓN (apagar los bonus/malus de threatened-by-lesser
  en atomic y medir); si la eliminación gana o empata, segunda versión
  con amenaza-por-blast-rentable.
- **U8 exploratorios** (item "plenty more"): history de quiets en anillos
  de reyes / piezas fuertes; modulación de poda con reyes conectados
  (capturas junto a reyes ilegales → densidad táctica baja). Notas de
  diseño primero, implementación tras U1-U7.
- **U9**: ola SPSA sobre las constantes nuevas al asentarse la tanda.

## Protocolo (obligatorio)

- Base: `fable/mv1-futility` (c9edd7de). Ramas `UB<n>-<slug>` INDEPENDIENTES
  desde base (no apiladas). Bench limpio por rama (receta ob-test-cloning).
- SPRT: STC 8+0.08 [0.00, 3.00] cap 20k → LTC 40+0.4 [0.00, 2.50] cap 12k
  contra la base vigente al pasar.
- **Mandato de persistencia (del propio ubdip): nada de one-shots.** Cada
  idea recibe 2-3 parametrizaciones con post-mortem entre intentos antes de
  archivarse; el post-mortem se escribe (dossier por idea).
- Orden de lanzamiento: U1 (neutral) → U2/U3/U4 (núcleo) → U6/U7 → U5 → U8.

## Ola MV-R15+: mates rápidos (3-ago, informe de Wolfram → fase A medida)

Wolfram midió a mano que MV-SF release 10 (¡2018!) encuentra mates MUCHO
más rápido que Atomic-Stockfish (sus 4 tests en #solver). La fase A del
laboratorio lo convirtió en programa con recibos:

- **Suite de 84 mates probados de atomicdb**: MV-SF-2018 **83/84**
  (mediana 201 ms, 0 FENs rechazados); nuestra base **74/84** (238 ms).
  Pistola humeante P10: mate-en-11 por jugadas tranquilas con jaque
  quieto — MV-SF 66 ms @ d15, nosotros ciegos a 20 s con más profundidad.
  **Fallo de PODA, no de profundidad.**
- **El "bug" del test 4 de Wolfram: explicado, no es bug** — la búsqueda
  del bando perdedor degenera (2,68 re-búsquedas/iter vs 1,15; 8,9× nodos
  a igual profundidad) y la red de mate pasa por jaques quietos podados.
  El ajuste TT de mates es stock verbatim (auditado).
- **Matriz de ablaciones** (suite + 4 tests duros): jaques quietos en el
  primer ply de qsearch **77/84 y −20% de árbol**; movecount-pruning
  nunca sobre jaques quietos **76/84, +2/−0 monótono, −27% de árbol**;
  `AtomicMcpBase` 7→20 **76/84, T2 de 58 s→17 s, cero código**. La
  extensión de jaque de 2018: +117% de árbol, DESCARTADA. El guard de
  NMP en banda de mate ya es correcto (ablación = no-op exacto). Los
  combos: sub-aditivos, peores que sus piezas — no combinar.
- **Lanzados**: T159 `MV-R17-mcpbase-20` (solo-opción, prio 204→203 al
  entrar la ronda 3 spell por delante); MV-R15 y MV-R16 implementados
  con el **gate de crash-smoke** nuevo (80 partidas vs base, atribución
  por lado — nacido del incidente spell de hoy) y **descartados por el
  gate de mates** (post-mortem abajo).
- Artefactos permanentes: `C:\mate-lab\matebench.py` + suite de 84; las
  ablaciones viven como spins UCI en el binario del lab (verificado
  nodo-a-nodo idéntico a la base con todo a 0).

**CORRECCIÓN (4-ago): la "base equivocada" del post-mortem de abajo no
era equivocada.** `c9edd7de` (MV-R4) pasó STC (T107, +2,95) y LTC (T119,
+2,97) el 28-29 jul: era la punta LEGÍTIMA del dev, y el 4-ago aterrizó
en main vía PR#52 (merge `e4acf215`, bench 518466, unit tests 104/104).
Lo que sí sostiene el post-mortem: los deltas del lab medidos contra
c9edd7de no eran atribuibles a los candidatos, y la sonda de coste
invertía el signo. **Desde ahora la base de registro de tests atomic es
main (`e4acf215`, bench 518466)**; T159/T163 siguen siendo A/B válidos
(dev y base comparten linaje pre-MV-R4 los dos).

### Post-mortem R15/R16 (3-ago noche): los recibos del lab no reproducen

Intento 1 del mandato de persistencia — archivado con causa raíz, no
por pereza. Tres hallazgos del implementador (agente, verificado):

1. **El lab medía contra la base equivocada.** `C:\mate-lab` estaba en
   `c9edd7de` (MV-R4, ensanche de futility) — un commit de laboratorio
   con **+81% de árbol** y 2 mates MENOS que la base de flota real
   `f6c39698` (spsa90). Todos los deltas "−20%/−27% de árbol, +2/+3
   mates" eran relativos a esa referencia inflada: recortar 27% de ahí
   sigue cayendo un 14% por encima de la base que juega la flota.
2. **La ablación a8 nunca buscó un jaque quieto.** Su filtro corría
   antes del `if (!capture) continue;` del paso 6 de qsearch, así que
   solo midió el cambio de etapa del MovePicker (orden de capturas).
   Réplica literal sobre base de flota: 76/84, +6% de árbol — neutra.
3. **Las implementaciones honestas sobre `f6c39698` pierden.** R15
   (jaques quietos de verdad en el 1er ply de qsearch + TT a dos
   niveles, commit `78edb262`): 74/84 y **+75% de árbol**. R16
   (movecount-skip en jaques quietos, `1fdb1d03`): 74-73/84 y +14%.
   Base: 75-76/84. Ambas crash-smoke limpias (0/0 en 80).

**Decisión: sin SPRT para R15/R16** — pierden la métrica para la que
nacieron. Ramas conservadas en `C:\mvr-lab-r15`/`-r16`/`-probe` (nada
pusheado). El recibo de T159 sale del mismo lab contaminado, pero su
SPRT es árbitro limpio: que decida solo.

Lecciones: (a) todo lab pina la base de FLOTA (`f6c39698`), no el HEAD
que convenga; (b) el hueco real sigue ahí — MV-SF-2018 83/84 vs
nuestros 75-76/84, medido con binario MV-SF independiente — pero no se
cierra AÑADIENDO jaques sin ordenar, que es coste sin beneficio; el
intento 2 debe atacar el ORDEN (history de jaques, jaques primero en
la etapa) y re-derivar las ablaciones sobre la base de flota antes de
tocar código. (c) `make atomic-unit-tests` lleva roto en la base desde
spsa90 (11 expectativas rancias en `tests/atomic_see.cpp`) — tarea
aparte ya señalizada.

### Intento 2, fase de medición (3-ago noche): la matriz re-derivada

Lab `C:\mvr-lab-abl` (rama `mate2/abl-matrix`), binario de ablaciones
reconstruido sobre `f6c39698` con gate superado (todo a 0 = bench
213680 y nodo-a-nodo idéntico a base). Veredictos:

- **Suelo de ruido de la suite: ±2 mates.** `a2` (guard NMP en banda de
  mate) es no-op EXACTO — árbol bit-idéntico — y puntúa 77/77 contra
  base 76/75. Ninguna celda de la matriz bate a la base con esa regla.
  Los conteos de mates de R15/R16 (74 vs 75-76) también caen dentro del
  ruido: su rechazo se sostiene en el COSTE de árbol en bench
  (+75%/+14%), que sí es determinista.
- **Segundo bug de medición del intento 1** (independiente de la base
  equivocada): la sonda de coste de 4 posiciones INVIERTE EL SIGNO —
  reportó a8R a −43% donde bench dice +25,1%. Todos los "−20%/−27%/
  +117%" del intento 1 colgaban de esa sonda. Regla nueva: **el coste
  se mide en `bench`, jamás en la sonda estrecha.**
- **En bench, TODAS las ablaciones cuestan árbol; ninguna ahorra.** Y
  las "ganancias" del intento 1 eran auto-reparación: recuperaban
  P10/P76/P81, mates que la base de flota YA resuelve (deshacían el
  ensanche de futility de MV-R4, no encontraban nada).
- **R16/a7 reimplementaba algo que ya existe**: la base retiene jaques
  quietos bajo movecount-pruning (`movepick.cpp:305-320` compacta los
  `gives_check` cuando `skipQuiets`); lo único que cambiaba era su
  ORDEN, a +41,6% de bench por cero mates.
- **Curva AtomicMcpBase (recibo para T159)**: 7→12→20→28→40 = +0/+5/
  +26,5/+13,1/+30% de bench; en mates es un TRUEQUE (compra largos
  24-30 plies, vende cortos) con ceguera real: a mcp 20/28 el motor
  llega a d25-27 sin anunciar mates-en-6 que la base anuncia a d12-18
  (P10, P25; réplicas idénticas). Leer el SPRT de T159 con esto en la
  mano.
- **La brecha 75→83, localizada**: exclusivamente mates largos (≤14
  plies 35/35 ambos; ≥20 plies nosotros 29/36, MV-SF 35/36). No es
  profundidad (en P51/P60/P72/P79 llegamos MÁS hondo que MV-SF y no lo
  vemos). P72: mate-en-10 de jugadas TODAS quietas que aterrizan en el
  anillo del rey enemigo — las casillas absolutamente inmunes de
  atomic (capturar ahí explota su propio rey) — y el orden de quiets no
  tiene hoy NINGÚN término para eso (`see_ge` lo sabe como "no pierde",
  nunca como señal positiva).

**Dossier del intento 2, rankeado** (implementación en curso de #1-#2):
1. `king-ring quiet bonus` (movepick.cpp:233-234): bonus TUNE a quiets
   cuyo destino cae en el anillo del rey enemigo; un AND por quiet;
   0 = bench bit-idéntico. Ataca la única señal que comparten las PVs
   que fallamos.
2. Predicado de jaque atómico-correcto en el bonus de quiets: la línea
   234 usa `check_squares` (falla 3 veces en atomic: rey hardcoded 0,
   descubiertas invisibles, falsos positivos con reyes adyacentes);
   el sitio de retención 313 ya usa el correcto `gives_check`. Fix:
   `gives_check` también en 234.
3. Puntuar/ordenar los jaques retenidos del bulk path (hoy value=0 sin
   sort). 4. `checkHistory` butterfly (tras probar que #2/#3 disparan).
5. `AtomicMcpSlope` (2018 tiene ~2× nuestro presupuesto de quiets por
   PENDIENTE, no por base — ningún AtomicMcpBase lo compensa; ir
   sabiendo que la curva dice trueque). 6. Orden de capturas por
   blast-SEE = redescubrimiento independiente de U3 (necesita U1).
7. Etapa checks-first en banda de mate (última: a2 no-op, a5 perdedora).

**Gates para CUALQUIER SPRT de mates** (con dos bugs de medición
encontrados, obligatorios): bench idéntico a knob 0; contador que
pruebe que la ruta dispara; nodes-to-mate del subconjunto ≥20 plies
(n=36, determinista — los conteos de la suite no resuelven mejor que
±2); coste SIEMPRE en bench; y ensanchar la suite (los 6 mates que
faltan vienen de UNA familia de partidas) antes de creerse candidato
alguno.

### Intento 2, fase de implementación (3-ago noche): #1 muere, #2 a cola

TERCER bug de medición cazado antes de firmar nada: el harness de
nodes-to-mate solo contaba `score mate > 0`, y **24/36 posiciones del
subconjunto ≥20 tienen al DEFENSOR al mover** (mate negativo) — tiraba
dos tercios del set en silencio. Doctrina: contar ambos signos y
reportar SIEMPRE el split atacante/defensor (los dos candidatos
resultaron ~30% más baratos atacando y más lentos reconociendo el mate
propio; pooled = moneda al aire que esconde eso).

- **#1 king-ring bonus (`mate2/king-ring-bonus`, d9ebae4): ARCHIVADO
  por sus propios recibos**, con los gates formales en verde. La
  instrumentación que exigimos fue la que lo mató: los bestMove-quiets
  al anillo ya salen a rango medio 1,35 (58% primeros de serie, 2,29×
  enriquecidos) — no hay margen que comprar — y el término degrada a
  los bestMove fuera del anillo (1,53→1,59). Nodes-to-mate pooled
  plano a 3 knobs. Si algún día se relanza: knob 3000, único valor no
  peor que base en la suma. El proceso funcionó: cero flota gastada.
- **#2 predicado de jaque atómico (`mate2/atomic-check-predicate`,
  405bb59): → T163** (STC, prio 203, bench registrado 283285). Fix de
  corrección de una línea (`gives_check` donde el scoring usaba
  `check_squares`, ciego a jaques de rey y descubiertas y con falsos
  positivos de reyes adyacentes — la retención de la línea 313 ya
  usaba el correcto). Censo completo: 2,62% de quiets en desacuerdo,
  100% de FPs explicados por adyacencia de reyes, y los FNs de rey son
  el 56% de los desacuerdos-bestMove (los jaques invisibles eran la
  jugada buena). Coste real −6,5% nps que debe recuperar en orden;
  atacante geomean 0,667 (p=0,039 post-hoc, sugerente NO establecido).
  El bench canónico +32,6% es lotería de profundidad (d10-d17 agregado
  −0,9%; 84 FENs a d14 −15,8%) — leer el número de OB sabiéndolo.
- **Gate de coste REDEFINIDO**: el bench canónico de 13 posiciones
  oscila ±39% profundidad-a-profundidad ante cambios de ORDEN con
  árbol agregado plano — sustituye a la sonda invertida de 4 pero
  sigue infrapotenciado. Desde ya el coste se juzga con el agregado
  d10-d17 o con N ancho a profundidad fija (84 FENs d14); el canónico
  queda solo como firma de registro.
- Idea de Wolfram aparcada con cariño: modo-solve del motor (comprometerse
  con una blanca fuerte y romperla; NNUE de solving separada) — tras la
  ola SPRT.

Intel de referencia (ijhy en #solver, 4-ago): hubo MV-SF buenos del
13-sep-2019 y del 7-nov-2019 (este lo encontró Gannet); las versiones
de alrededor eran muy flojas. Si se consiguen esos binarios, la vara
del programa (hoy release 10 de 2018, 83/84) puede subir aún.

## Pendiente de responder a ubdip

Su pregunta de royalty ("more like mv-sf than fsf?"): nuestra
implementación es propia sobre el chasis SF-dev con FSF como oráculo de
reglas (perft-validada); reyes conectados/explosión implementados en
movegen+do_move nativos, no vía extinción genérica FSF.
