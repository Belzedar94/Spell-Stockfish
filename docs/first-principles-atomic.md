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
- **Presupuesto de quiets por PENDIENTE** (`AtomicMcpSlope`): la diferencia
  estructural MEDIDA con MV-SF-2018 (~2× quiets por profundidad; ningún
  AtomicMcpBase lo compensa — la curva base 7→40 es un trueque con ceguera
  real a mcp 20/28, recibos en el doc del programa). Pendiente, no base; y
  modulada por reyes-conectados (física 3: densidad táctica local colapsa →
  presupuesto abajo cerca del contacto).
- **statScore por blast** (U4/search.cpp:1641) y **threatened-by-lesser
  re-derivado** (U7: primera versión = eliminación, medida ya hecha en
  T137 −0,41 — la eliminación sola no paga; la versión blast-rentable entra
  aquí).
- Null-move y su verificación en banda de mate: auditados correctos (a2 fue
  no-op exacto); no se tocan.

**Secuencia**: R-B se implementa SOBRE el MovePicker nuevo — el orden
correcto cambia qué podas disparan; medir R-B contra el orden viejo sería
medir otra cosa.

## R-C. blast_see como único oráculo táctico

La fundación (U1) que falló como trocito: T131 −1,93 pese al diseño
"bit-idéntico en NORMAL". **Autopsia en curso (bloqueante)**: harness de
paridad de veredicto por clases (NORMAL / promo / EP / reyes-conectados /
umbral negativo) sobre bench+perft+FENs de anillos cargados. Hipótesis
ordenadas: (a) la extracción rompió la paridad en NORMAL (bug), (b) la
semántica nueva de promo/EP tiene el signo mal respecto al do_move real,
(c) la semántica es correcta pero los consumidores calibrados al proxy la
castigan (→ se resuelve DENTRO de R-A/R-B, no antes).

Con la autopsia resuelta: `blast_see(Move) -> Value` con test diferencial de
VEREDICTO (no solo de delta material) y TODOS los `PieceValue[captured]` del
motor muriendo A LA VEZ — futility (2 sitios), statScore, orden de capturas,
threatened-by. La ronda UB probó que de uno en uno son ruido; el paquete
redefine la lente táctica entera y se mide como tal.

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
