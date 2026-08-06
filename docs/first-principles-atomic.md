# Atomic desde first principles: el programa de reworks

Mandato del 5-ago-2026: "pruning, see, move ordering están optimizadas para
ajedrez clásico; no vamos a conseguir cientos de elo cambiando pequeños
trocitos". Este doc es el programa atomic completo. El de spell es
`first-principles-spell.md`; el índice, `first-principles-reworks.md`.

La evidencia que lo motiva ya está pagada: 13 SPRTs de knobs sitio-a-sitio
cerrados en neutro (±0-2 Elo reales) el 5-ago, spsa90 sentado en el óptimo
local de las constantes clásicas, y el mate-lab demostrando dos veces que un
bonus sobre el orden clásico no compra la brecha de mates (el king-ring bonus
murió con los bestMove ya a rango 1,35: el problema es la ESTRUCTURA que
decide qué compite con qué, no los pesos).

## 0. La física de atomic

1. **Capturar es explotar.** La víctima, el CAPTURADOR y todo el anillo 3×3
   salvo peones desaparecen. Corolarios: (a) no existe la recaptura → no
   existen las secuencias de intercambio que el SEE clásico modela con su
   swap; (b) el valor de una captura es un delta multi-pieza con signo
   cualquiera — "ganar la dama" puede ser −6 si el anillo se lleva torre y
   alfil propios; (c) el `see_ge` de la base ya es blast-aware
   (position.cpp:1382) pero es booleano-por-umbral y TODOS sus consumidores
   usan el proxy `PieceValue[captured]` (recibos en ubdip-search-program.md).
2. **El intercambio igualado pierde tempo** (ubdip item 9). Sin recaptura,
   una captura "equal" gasta el turno y asienta el tablero. En clásico es
   neutral-a-buena (abre líneas); aquí es estructuralmente sospechosa. ubdip
   no logró rescatar esto en MV-SF; el chasis moderno + SPSA son la segunda
   oportunidad.
3. **Reyes conectados = inmunidad absoluta.** Capturar adyacente al propio
   rey es ilegal (te explotas): dos reyes en contacto crean casillas inmunes
   y apagan la táctica local. Los mates largos que no vemos (≥20 plies:
   29/36 nuestros vs 35/36 de MV-SF-2018) son líneas QUIETAS que aterrizan
   en el anillo enemigo — la jugada letal no captura nada y el orden de
   quiets no tiene hoy NINGÚN término para eso (P72, matriz del intento 2).
4. **Morir es un evento, no una acumulación.** La derrota es UNA captura
   aterrizando junto al rey. "Peligro de rey" es un predicado de alcance de
   explosión, no un gradiente de ataque.
5. **Los peones son escudos.** Sobreviven a los anillos ajenos; la
   estructura de peones pegada al rey vale varias piezas. (La red lo
   aprende sola; la BÚSQUEDA no lo consulta para podar.)

## R-A. El MovePicker atómico

**Tesis**: las etapas clásicas (TT → capturas buenas MVV/LVA+SEE → killers →
quiets → capturas malas) son la política equivocada para 1-3. La nativa:

1. TT move.
2. **Amenazas de anillo**: jugadas — quietas O capturas — cuyo destino crea
   amenaza de explosión sobre el anillo del rey enemigo. El vector de mate
   de la variante, hoy invisible para las etapas.
3. **Blasts ganadores**, ordenados por `blast_see` con signo (R-C).
4. Killers / countermove.
5. Quiets por history, con el predicado de jaque CORRECTO en el scoring
   (la línea 234 de movepick usa `check_squares`, ciego a jaques de rey y
   descubiertas — el fix exacto es T163, en SPRT).
6. **Blasts igualados** (los pierde-tempo): DETRÁS de los quiets. El vuelco
   más contraintuitivo y el más atómico. `AtomicCaptureTempo` (U6) entra
   aquí como frontera de la etapa, no como malus suelto.
7. Blasts perdedores.

**Implementación**: una rama, spins UCI por etapa con 0 = clásico exacto
(verificado bench-idéntico); la etapa 2 necesita un predicado barato
"destino ∈ anillo enemigo ∧ crea amenaza de captura legal ahí" (un AND de
bitboards + un attackers_to en el destino). La etapa 6 necesita blast_see
(R-C) para distinguir igualado de ganador.

**Por qué como paquete**: el orden es un sistema de prioridades RELATIVAS —
mover una etapa sin mover las demás no cambia qué gana a qué; la ronda UB/SB
ya pagó la demostración de que los trocitos son neutros.

**Recibos antes de flota**: fire-rate por etapa; rango medio del bestMove
por clase (harness del intento 2, ya existe); subset de mates ≥20 plies con
AMBOS signos y split atacante/defensor; coste en bench d10-d17 (jamás el
canónico solo: ±39% de lotería ante cambios de orden). Y el ORÁCULO:
posiciones de atomicdb con jugada probada → time-to-correct-move (R-E fase
oráculo). Meta honesta: cerrar ≥la mitad de la brecha 29→35 del subset largo
sin coste neto en bench agregado.

### La línea de MV-SF del 13-sep-2019, medida SOLA: REFUTADA (6-ago)

Lab `capthist/mvsf-2019` en "Atomic Project\capthist-lab", sobre base pura
e4acf215, jamás registrado.

**La fuente, localizada y leída**: `a9344db3` "Use capture history to rank
atomic moves" (ddugovic/Stockfish, 13-sep-2019; padre `e47849b6`, 12-sep). El
diff es un fichero, una línea partida en dos: `m.value = pos.see<ATOMIC>(m) * 6`
pasa a `... * 6 + (*captureHistory)[moved_piece][to][type_of(piece_on(to))]`.
Su SPRT: STC LLR 2,98 en [-5,10] con 683 partidas, LTC 2,96 con 508. Y la
premisa reproduce en nuestras suites (5-ago, 3 reps, jobs 4, mt 20 s):
**mvsf-20190912 = 80/84 y 32/36 largos** (estable en las tres), **mvsf-20190913
= 83/82/82 y 35/34/34**. La línea vale de verdad +2/+3, ella sola.

**El hallazgo que cambia el problema: media línea YA ES NUESTRA.** movepick.cpp
:215-216 de la base pura puntúa `captureHistory[pc][to][víctima] + 7 *
PieceValue[víctima]`, y `CapturePieceToHistory` tiene el MISMO tope ±10692 en
los dos motores. Lo que MV-SF añadió aquel día ya lo tenemos. La diferencia
viva no es la historia: es el término MATERIAL. El de MV-SF es
`see<ATOMIC_VARIANT>`, el delta CON SIGNO de la explosión (capturador en
`from` + víctima + anillo no-peón); el nuestro es la víctima BRUTA ortodoxa.
Portar la línea honestamente = cambiar la CANTIDAD, no añadir la historia.

**Implementación** (80 líneas insertadas, 42 de código): `Position::
blast_capture_value` expone el mismo `result` que `see_ge` (position.cpp:1382)
ya calcula para capturas — misma precedencia de reyes, mismo −1 de tempo;
promo/EP devuelven 0 porque esa política es R-C y no viaja de rondón — y
`AtomicMpBlastMat` es su coeficiente, con 0 = base **bit-idéntica**
(canonical 518466, d10-d17 7.563.584, wide84@d14 4.191.481, diff vacío).
Cross-check de que no estamos midiendo otro oráculo: bv tiene que ser el punto
exacto de volteo de `see_ge` — **393.899 sondas, 0 desacuerdos**.
Traducción de escala con el tope de historia fijo: los MG atómicos de MV-SF
(P244 N437 B552 R787 Q1447) corren 1,34× por debajo de nuestro
`AtomicCapturePieceValue` (P301 N702 B738 R1079 Q1862), así que su 6 aterriza
en **≈4,5** aquí; 7 es el otro punto natural (nuestro propio coeficiente,
cambiando solo la cantidad). Barrido 3 / 5 / 7 / 11.

**El término dispara, y fuerte** (censo MP_AUDIT, bench d13 + 3 FENs largos a
depth 20): 22-27% de las listas de capturas se reordenan, 26-36% de las
capturas cambian de rango, 13-19% de las listas cambian su PRIMERA captura y
6-18% cambian de lado en la puerta `see_ge(m, −value/18)` — que se alimenta del
propio score. Y el diagnóstico físico crudo: **65-75% de las capturas tienen
delta de blast NEGATIVO** (la base las ordena todas como ganancias) y 4-9%
**explotan un rey**. No es un no-op como C2: es medio tablero.

**Y aun así pierde.** Cinco brazos en la MISMA tanda, jobs 2, movetime 20 s,
3 reps intercaladas (252 intentos por brazo):

| brazo | 84×3 | por rep | ≥20×3 | <20×3 | d10-d17 | wide84@d14 |
|---|---|---|---|---|---|---|
| **C0 base** | 239/252 | 79,67 | **95/108** | 144/144 | — | — |
| C3 | 234/252 | 78,00 | 90/108 | 144/144 | +3,9% | −16,7% |
| C5 (≈MV-SF) | 238/252 | 79,33 | 94/108 | 144/144 | −9,3% | +21,1% |
| C7 | 237/252 | 79,00 | 93/108 | 144/144 | −0,9% | +23,4% |
| C11 | 232/252 | 77,33 | 88/108 | 144/144 | +3,9% | +24,9% |

El listón era ≥+2 largos sobre 3 reps sin coste neto >+10% en d10-d17: **el
coste pasa en los cuatro brazos y ninguno llega siquiera a la paridad**. Sin
dosis-respuesta ascendente en 3→5→7→11; el óptimo de la familia es el 0.

**El trueque, posición a posición** (cuentas de 3 reps): compra **P76** (29
plies) de forma determinista — 0/3 en la base, **3/3 en C5, C7 y C11** — y
vende P51 (24, def: 3→1/0/0), P57 (22, def: 3→0 en C3/C7/C11), P58 (25, atac:
2→0 en C5) y P68 (29, atac: 3→0 en C11). **P76 es exactamente la posición que
la pendiente 128 también desbloqueaba**: dos mecanismos independientes abren la
misma puerta y los dos la pagan con el mismo tipo de mates. Los pareados van
todos por debajo de 1 (≥20 atacante ×0,79-0,91, defensor ×0,70-0,90): árbol más
barato que resuelve menos, con el adorno de supervivencia de siempre.

**Oráculo de atomicdb** (hallar_ganadora, 1.998 posiciones, pareado con
McNemar, C5 contra base): 2k **1.376 vs 1.375, p=1** (empate exacto); 32k 1.554
vs 1.527, p=0,053; 512k 1.591 vs 1.566, p=0,049 — es decir, ligeramente EN
CONTRA a presupuesto alto, nada a favor en ninguno. Bucket largo sin diferencia
(p=0,44/0,46). Ni siquiera queda el hallazgo lateral de calidad de datos que sí
dejó la pendiente (+2,2 puntos a 2k, p=0,003).

**Por qué no porta** (la lección, no la excusa): en MV-SF-2019 la línea AÑADÍA
señal — el orden de capturas era material puro y la historia entró a
corregirlo. Aquí la historia ya está, y lo que el brazo hace es QUITARLE el voto
al proxy ortodoxo para dárselo a un delta de explosión materialmente honesto,
que además realimenta su propia puerta de buenas/malas. Es la firma de
T131/U1 otra vez: material exacto sustituyendo a una historia que ya había
aprendido el sesgo correcto. **Un motor sin la mitad histórica mejora con el
delta con signo; el nuestro ya paga por él.** Corolario de método: antes de
portar un diff histórico, comprobar cuál de sus mitades ya vive en la base —
el mismo parche es una mejora o una sustitución según lo que haya debajo.

**Consecuencias para R-A**: (1) la etapa 3 ("blasts ganadores ordenados por
`blast_see` con signo") queda medida SOLA y en rojo sobre base pura; si
sobrevive es dentro del paquete y con la puerta buenas/malas re-cableada — un
umbral que no se derive del propio score —, jamás como término suelto; (2) el
65-75% de capturas con delta negativo es el recibo que sostiene el vuelco de la
etapa 6: esas capturas hoy compiten como ganancias puras; (3) `blast_capture_
value` queda construido y verificado contra `see_ge`, listo para los
consumidores de R-B sin volver a pagar la fundación.

## R-B. Podas conscientes de la explosión

**Tesis**: todo margen de poda presupone "lo máximo que gana esta jugada ≈
PieceValue[víctima]" y "una posición quieta no esconde saltos". En atomic el
salto máximo es el blast entero y la quietud no existe cerca de anillos
cargados.

- **Futility/delta/probcut por blast**: search.cpp:1486 y :2090 suman
  `PieceValue[captured]`; pasan a `blast_see(move)` (U2, que murió SOLO —
  aquí entra con sus consumidores y sus constantes re-SPSA).
- **Estado "bajo amenaza de blast"** como análogo de `in check` para las
  puertas: no futility/movecount-prune cuando una pieza enemiga no-peón
  tiene captura legal adyacente a nuestro rey. Predicado barato precomputado
  por nodo; contador de "podas evitadas" para atribución.
- **Presupuesto de quiets por PENDIENTE** (`AtomicMcpSlope`): **REFUTADO
  6-ago con recibos** (lab `slope/mcp-curve` en "Atomic Project\slope-lab",
  jamás registrado). La premisa sobrevive y queda citada en fuente: nuestra
  base `(7+d²)/(3−improving)` da d²/2 improving; MV-SF sep/nov-2019
  `(5+d²)(1+improving)/2` da d² — **2× exacto**, y ningún `AtomicMcpBase` lo
  expresa. Lo que muere es la inferencia: en NUESTRO motor la pendiente
  **vende** mates largos de forma MONÓTONA — slope 64/96/128 = **91/87/85**
  largos sobre 108 intentos (5 brazos × 3 reps, jobs 2, mt 20 s, ancla
  oscilando solo 78/78/79). El mecanismo predicho SÍ existe y es
  determinista (P76, 29 plies, 0/3 → **3/3**, solo con slope 128) pero se
  paga con P68, P79 y P10 enteros: el mismo trueque de la curva base a otro
  precio. Coste sí pasaba (+2,4% d10-d17, −7,6% wide84) y el pareado
  favorable (×0,867 atacante) está adornado por supervivencia — el brazo
  tira justo las más duras. Diagnóstico: **la pendiente no es portable, es
  el complemento de un ORDEN que no tenemos** (el diff 12→13-sep-2019 de
  MV-SF es UNA línea de movepick — captHist sumado al `see*6` atómico — y
  vale 80→83/84 él solo: material de R-A). Con tres puntos y dosis-respuesta
  limpia la familia se cierra sin tercera parametrización.
  - **Seguimiento de esa pista, cerrado el mismo 6-ago**: la línea se portó y
    se midió sola sobre base pura → **también roja** (post-mortem en R-A). Su
    mitad de historia ya era nuestra; la mitad material que faltaba compra el
    mismo P76 que la pendiente y vende los mismos mates largos. Las dos vías
    hacia el orden de MV-SF-2019 están ahora refutadas por separado: lo que
    queda es el PAQUETE R-A, no ninguna de sus líneas.
  - **El tope de 2018 (`depth < 16`), enterrado gratis**: era el guard del
    array `FutilityMoveCounts[2][16]` de SF10, no una política. Contador:
    271.692 llamadas del paso 14 a d≥16 y **0 disparos evitados** — a esa
    profundidad el umbral base ya vale 87-131 y el MCP no dispara nunca.
    Brazo bit-idéntico a la base; nadie tiene que volver a mirarlo.
  - **Modulación por reyes conectados: DOBLE-REFUTADA.** 50 fue guillotina
    (5-ago); 85 vende P57 entero y da ×1,295 de nodos donde duele. Cerrada.
  - **Hallazgo lateral con recibo, no reclamado como Elo**: con slope 128 la
    calidad de decisión contra teoremas de atomicdb sube a **2.000 nodos**
    (68,9%→71,1%, McNemar p=0,003; bucket largo 55,3%→59,9%, p=0,005) y el
    efecto **es cero a 32k** (450 vs 450 exacto). Palanca de calidad de
    DATOS para datagen a presupuesto bajo, no de fuerza del motor.
- **statScore por blast** (U4/search.cpp:1641) y **threatened-by-lesser
  re-derivado** (U7: primera versión = eliminación, medida ya hecha en
  T137 −0,41 — la eliminación sola no paga; la versión blast-rentable entra
  aquí).
- Null-move y su verificación en banda de mate: auditados correctos (a2 fue
  no-op exacto); no se tocan.

**Secuencia**: R-B se implementa SOBRE el MovePicker nuevo — el orden
correcto cambia qué podas disparan; medir R-B contra el orden viejo sería
medir otra cosa.

**Recibos C2-solo (5-ago, re-medición sobre base pura): NO paga.** Tres
hallazgos con recibos:

1. **Sobre la base, C2 es UN knob, no dos**: `AtomicSeeDisc=1` entra solo
   por `blast_see_rel()` (position.cpp:1497), consumido únicamente en la
   rama `AtomicMpBlastOrder==2` (movepick.cpp:291). Sin R-A la descubierta
   no cambia un nodo (bit-idéntico, 0 llamadas en el censo). C2-sola ≡
   `AtomicMcpConnected=50` sola.
2. **Veredicto de mates, determinista (3 reps byte-idénticas, jobs 2)**:
   base pura 80/84 y 32/36 largos; C2-sola 77/84 y 30/36 — pierde P51
   (24 plies, defensor) y P58 (25, atacante), gana ninguna. El −17,4% de
   árbol d10-d17 se compra podando soluciones reales: nodos pareados ≥20
   plies ×1,027 (más lenta donde duele), <20 ×0,906. Trueque invertido de
   la curva mcp. No cumple el criterio "≥+2 largos sin coste" → sin rama
   mínima, sin SPRT.
3. **El recibo histórico "78/84, 30/36" de C2-sobre-R-A NO reproduce**: era
   dependencia de jobs 4 (P79 entrando/saliendo); a jobs 2 el +1/+1 se
   invierte a −1/−1. Además R-A queda BAJO la base pura aquí (78 vs 80),
   coherente con T166 en negativo. Doctrina: comparaciones de mates solo
   entre brazos corridos bajo condiciones idénticas en la misma tanda.

**Dependencia viva**: si T166 (RA1 mínima) muere, `AtomicSeeDisc` se queda
sin consumidor — la descubierta necesitaría re-cablearse a un sitio del
oráculo que la base pise (futility/statScore de R-B), no viajar como mitad
de C2. Intento 2 del mandato de persistencia sería `AtomicMcpConnected`
75-85 (50 es el único punto muestreado de 20-200, mitad seca del
presupuesto en 18% de nodos), pero con signo hoy negativo no alcanza el
listón de entrada de ≥3 Elo previstos: aparcado salvo que T166/T167
reviva la familia.

## R-C. blast_see como único oráculo táctico

La fundación (U1) que falló como trocito: T131 −1,93. **AUTOPSIA RESUELTA
(5-ago, rama `ub1-autopsy` en C:\at-ub1): veredicto (c)**, con (a) y (b)
refutadas con recibos:

- La extracción es BIT-IDÉNTICA en NORMAL/promo-quieta/enroque: 0
  desacuerdos en 2.270 millones de comprobaciones estáticas y 13,38M de
  llamadas en árbol real. La premisa del refactor está probada; blast_see
  puede ser el oráculo único.
- El diferencial contra do_move/undo_move da 0 discrepancias en 24,4M de
  capturas: la semántica promo/EP es materialmente EXACTA.
- Lo que mató el SPRT: en este motor `see_ge(m, thr>0)` solo aparece
  cuando la captureHistory se ha ido muy negativa. La base devolvía 0 en
  promo-captura/EP → fallaba siempre con umbral positivo → MANDABA LA
  HISTORIA. UB1 devuelve el material real y pasa: le quita el voto a una
  historia que YA HABÍA APRENDIDO que la promo-captura atómica es mala
  (el 57% llegan a la puerta con captHist < −7·PieceValue). 0,057% de
  vuelcos de veredicto (93% en dirección despodar) → +15,7% de árbol
  d10-d15. Materialmente verdad, posicionalmente optimista: blast_see
  cobra el peón gastado a 301 cuando es un pasado en 7ª, y la pieza
  promovida vale 0 porque explota con su propio blast.
- **Bonus, bug preexistente de la BASE**: movepick.cpp:215 puntúa al paso
  con `PieceValue[piece_on(to)]` = casilla VACÍA → 0 → la puerta cae en 0
  exacto, justo donde `blast_see(EP) = −1` invierte; el al paso queda
  además ordenado el último de todas las capturas. Se arregla DENTRO de
  R-A (etapa 6 lo heredaría calibrado en 0 para una clase entera).

**Consecuencias de diseño para R-C/R-A**: (1) la extensión promo/EP es un
CAMBIO DE POLÍTICA disfrazado de fundación — entra como spin propio del
paquete (0 = base bit-idéntica, verificable con el harness `seeaudit` que
la autopsia deja construido), jamás de rondón; (2) el conflicto
material-vs-historia en umbral positivo se decide EXPLÍCITAMENTE antes de
cablear consumidores — reaparecerá en U2/U3/U4; (3) la etapa 3 del
MovePicker no quiere el delta crudo sino el delta contra el valor de la
pieza GASTADA (peón en 7ª ≠ 301) — un blast_see relativo-a-inversión.
Con eso: TODOS los `PieceValue[captured]` mueren A LA VEZ dentro del
paquete R-A/R-B, medidos como paquete.

## R-E. La red con verdad absoluta

atomicdb contiene millones de posiciones con valor TEÓRICO PROBADO (cierres
decisivos con distancia; tablas por repetición probadas por la cascada
consciente de ciclos). Ninguna NNUE del ecosistema se ha entrenado nunca
contra verdad de juego — todas aprenden de opiniones de búsqueda.

- **Fase oráculo** (barata, sin flota): (1) suite de calidad de decisión
  para R-A/R-B — posiciones UNKNOWN con hijo probado ganador: ¿lo juega el
  motor, y a qué profundidad/tiempo?; (2) benchmark de eval — % de cierres
  probados que la red actual evalúa con el SIGNO correcto, por profundidad
  de la prueba y por distancia al mate. El número que salga es el techo
  visible y la línea base de todo lo demás.
- **Fase entrenamiento** (experimento GPU): fine-tune con posiciones de la
  frontera probada etiquetadas por la prueba (WDL exacto, λ hacia
  resultado), mezcladas con datos normales para no colapsar la
  distribución. Gate: sube el signo-sobre-probadas SIN regresión en los
  gates de fuerza a 3 TCs.
- Es también la respuesta ofensiva a la amenaza math_god (MVSF+NNUE
  moderna, "will probably destroy fairy and atomic-stockfish"): su red
  aprendería de búsquedas; la nuestra, de teoremas.

## Método común

1. Doc de diseño por rework: física → política → predicciones falsables.
2. UNA rama coherente por rework; spins UCI por componente; 0 = clásico
   bit-idéntico verificado con bench antes de nada.
3. Recibos ANTES de flota: gates del mate-lab redefinidos (bench d10-d17 o
   84-FENs a profundidad fija; subset ≥20 plies ambos signos; fire-rate con
   contador) + oráculo atomicdb.
4. SPRT del PAQUETE en banda ancha [0,5]: buscamos decenas, no décimas; lo
   que no resuelve rápido en banda ancha no es el rework que buscamos.
5. SPSA de las constantes nuevas SOLO tras pasar el paquete.
6. **Destilación** (el playbook del +55 de la expansión táctica): el árbol
   nuevo genera los datos que fijan la mejora en la red — λ1,0 para lo
   re-etiquetado, juzgar a 3 TCs, saturación esperada y aceptada.

## Orden de ejecución

1. Autopsia T131 (en curso) — bloquea R-A etapa 3/6 y todo R-C.
2. R-E fase oráculo en paralelo (tooling, no compite por flota).
3. R-A completo con recibos → SPRT paquete.
4. R-B sobre R-A → SPRT paquete.
5. SPSA conjunto de constantes nuevas; destilación; re-medir el listón de
   mates contra MV-SF-2018 (y contra los MV-SF de sep/nov-2019 si los
   binarios aparecen — intel ijhy).

Supervivientes de la era de knobs que siguen su curso y se integran si
pasan: U5/T135 ring-history (+1,63, la única señal clara de la ronda — es
de hecho el embrión de la etapa 2 de R-A), U3/T133, T152/SB9, T117, LTCs
de MV-R4, T163 (que ES el fix del predicado de R-A etapa 5), T159 (leer
con la curva mcp en la mano).

## Apéndice: los dos items de ubdip que faltaban (releído 5-ago, verbatim)

Contraste del doc contra el feedback original completo (29-jul): seis de sus
ocho puntos ya estaban mapeados (futility→R-B, threat-by-lesser→R-B,
ring-history→U5/T135, promos-EP→R-C, SEE0-tempo→R-A etapa 6, statScore→R-B,
reyes conectados→R-B). Dos NO estaban y son estructurales:

- **R-C+: amenazas descubiertas de explosión en el SEE** ("considering
  discovered king explosion threats is a bit more fancy, but could be quite
  feasible"). blast_see hoy valora el evento; la extensión valora lo que la
  jugada ABRE: mover una pieza puede liberar un sniper sobre casilla
  adyacente a un rey (amenaza letal creada) o des-bloquear la del rival.
  Entra como spin propio de R-C tras el paquete base — mismo trato que
  promo/EP: política explícita, jamás de rondón.
- **R-A etapa 2 generalizada: anillos de piezas FUERTES, no solo reyes**
  ("quiet moves in rings around kings or strong pieces"). Un quiet junto a
  la dama enemiga prepara un blast rentable aunque no toque al rey; el
  predicado de la etapa 2 se parametriza por conjunto de objetivos (reyes
  = mate; pesadas = material) con pesos separados. La versión reyes-solo
  va primero; la generalización es su segunda parametrización natural
  bajo el mandato de persistencia.
