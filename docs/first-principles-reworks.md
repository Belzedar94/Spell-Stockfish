# Reworks desde first principles (mandato del 5-ago-2026)

Encargo del propietario, literal: "hay grandes partes del motor como pruning,
see, move ordering, etc. que están optimizadas para ajedrez clásico, no vamos
a conseguir cientos de elo solo cambiando pequeños trocitos, necesitas pensar
en grande". Este doc es el mapa: la física de la variante primero, la
política que se deduce de ella después, y cada rework como UN programa
coherente con sus recibos — no una lluvia de knobs.

La evidencia que lo respalda ya la tenemos pagada: el triaje del 5-ago cerró
13 SPRTs neutros de knobs sitio-a-sitio (± 0-2 Elo reales), y el mate-lab
demostró dos veces que un bonus encima del orden clásico no compra la brecha
(el king-ring bonus murió con los bestMove ya a rango 1,35 — el problema no
es el peso, es LA ESTRUCTURA que decide qué compite con qué).

## 0. La física de atomic (lo que el chasis clásico asume mal)

1. **Capturar es explotar**: se va la víctima, el CAPTURADOR y todo el anillo
   3×3 salvo peones. No existe la recaptura; no existen las secuencias de
   intercambio. El SEE clásico (swap en una casilla) modela un juego que no
   es este.
2. **El intercambio igualado pierde tempo** (ubdip, item 9): sin recaptura,
   una captura "equal" gasta un movimiento y deja el tablero asentado. En
   clásico es neutral-a-buena (abre líneas, gana espacio); aquí es
   estructuralmente sospechosa.
3. **Reyes conectados = inmunidad absoluta**: capturar adyacente al propio
   rey es ilegal (te explotas). Dos reyes en contacto crean casillas
   absolutamente inmunes y apagan la táctica local. Los mates largos que no
   vemos (29/36 vs 35/36 de MV-SF en ≥20 plies) son líneas QUIETAS que
   aterrizan en el anillo enemigo — la jugada letal no captura nada.
4. **La condición de victoria es un evento, no una acumulación**: morir es
   que UNA captura aterrice junto a tu rey. "Peligro de rey" no es un
   gradiente de ataque clásico; es un predicado de alcance de explosión.
5. **Los peones son escudos**: sobreviven a los anillos ajenos. La
   estructura de peones alrededor del rey vale multiples piezas; el
   material lejos de los reyes vale menos que su tabla.

Cada pieza del chasis clásico — SEE, orden de capturas, futility, movecount,
null-move, extensiones — codifica los supuestos 1-5 AL REVÉS. spsa90 tuneó
las constantes de esas fórmulas; nunca pudo tunear sus FORMAS.

## R-A. El MovePicker atómico (orden como política, no como bonus)

**Tesis**: las ETAPAS del MovePicker clásico (TT → capturas buenas por
MVV/LVA+SEE → killers → quiets por history → capturas malas) son la política
equivocada para la física 1-3. La nativa:

1. TT move.
2. **Amenazas de anillo**: jugadas (quietas O capturas) cuyo destino crea
   amenaza de explosión sobre el anillo del rey enemigo — el vector de mate
   de la variante, hoy invisible para el orden (P72).
3. **Blasts ganadores** ordenados por `blast_see` con signo (necesita U1
   arreglado — autopsia T131 primero: paridad de veredicto promo/EP).
4. Killers/counter.
5. Quiets por history (con el predicado de jaque CORRECTO — T163).
6. **Blasts igualados** (los pierde-tempo): DETRÁS de los quiets, no
   delante. Este es el vuelco estructural más contraintuitivo y el más
   atómico de todos.
7. Blasts perdedores.

**Por qué como paquete**: cada etapa por separado ya demostró ±0-1 (ronda
UB/SB); el orden es un sistema de PRIORIDADES RELATIVAS — mover una pieza
sin mover las demás no cambia qué gana a qué. Se implementa con spins de
ablación por etapa (0 = clásico exacto, bench-idéntico), se mide el paquete.

**Recibos antes de SPRT**: fire-rate por etapa; rango medio del bestMove por
clase de jugada (el harness del intento 2 ya existe); subset mates ≥20
plies con ambos signos; coste en bench d10-d17. **Oráculo atomicdb**: suite
de posiciones con jugada PROBADA (cientos de miles de cierres decisivos en
la base) → time-to-correct-move. Ningún otro motor del mundo tiene esa
suite; usarla es pensar en grande con ventaja local.

## R-B. Podas conscientes de la explosión

**Tesis**: todos los márgenes de poda presuponen "el máximo que puede ganar
esta jugada ≈ PieceValue[víctima]" y "una posición quieta no esconde saltos".
En atomic el salto máximo es el blast entero (±varias piezas) y la quietud
no existe cerca de anillos cargados.

- Futility/delta/probcut: márgenes desde `blast_see` (U2 murió SOLO — como
  trocito; aquí entra como parte del sistema con sus consumidores).
- **Estado "bajo amenaza de blast"** como análogo de "in check" para las
  puertas de poda: no podar futility/movecount cuando el rey propio está en
  alcance de explosión enemiga (predicado barato: pieza enemiga no-peón
  ataca casilla adyacente a nuestro rey con captura legal).
- **Presupuesto de quiets por PENDIENTE** (`AtomicMcpSlope`): la diferencia
  estructural medida con MV-SF-2018 (~2× quiets por profundidad). La curva
  de mcp-base dice "trueque"; la pendiente es otra apuesta — más quiets
  DONDE la profundidad los amortiza, no en todas partes. Modulada por
  reyes-conectados (física 3: la densidad táctica local colapsa → presupuesto
  abajo) es ya política nativa, no constante.
- Null-move y verificación en banda de mate: ya auditados correctos; se
  integran sin tocar.

Mismos recibos que R-A + un contador de "podas evitadas por amenaza-blast"
para atribución.

## R-C. blast_see como único oráculo táctico (U1 completado, en paquete)

La fundación que falló como trocito (T131, −1,93: la extracción cambió
veredictos — autopsia pendiente ANTES de nada). Primer paso de R-A/R-B:
`blast_see(Move) -> Value` con paridad de veredicto probada
(test diferencial de VEREDICTO contra see_ge actual sobre las posiciones de
bench/perft, no solo de delta material), y TODOS los `PieceValue[captured]`
del motor muriendo A LA VEZ (futility, statScore, orden, threatened-by).
La ronda UB probó que uno a uno son ruido; el paquete redefine la lente
táctica entera y se mide como tal.

## R-D. Spell: la física primero, el rework después

Spell tiene su propia física (casts, piezas congeladas, el dialecto de
tablero extendido) y su ronda 3 de knobs igual de neutra. Pero su hueco
dominante es de EVAL (−74/−164 vs FSF+run5rl): ahí mandan datagen run9 →
run1c y la minería (lateral, diminishing returns aceptados). El ejercicio
first-principles de spell (¿qué asume el orden clásico que los hechizos
rompen?) queda como doc de diseño DESPUÉS de que el rework atómico enseñe
el método — misma plantilla, física distinta.

## R-E. La red con verdad absoluta (el activo que nadie más tiene)

atomicdb contiene millones de posiciones con valor TEÓRICO PROBADO
(cierres decisivos con distancia, tablas por repetición probadas). Ninguna
NNUE del ecosistema se ha entrenado nunca contra verdad de juego, solo
contra opiniones de búsqueda.

- **Fase oráculo** (barata, ya): suite de calidad de decisión para R-A/R-B
  (arriba) + benchmark de eval: ¿cuántos cierres probados evalúa la red
  actual con el signo CORRECTO? El número que salga es el techo visible.
- **Fase entrenamiento** (experimento): fine-tune con posiciones cercanas a
  la frontera probada etiquetadas por la prueba (WDL exacto, λ hacia
  resultado) mezcladas con datos normales. Si el signo-sobre-probadas del
  benchmark sube sin regresión de gates, hay una veta que nadie ha minado.
- Es también la respuesta ofensiva a la amenaza math_god (MVSF+NNUE): su
  red aprendería de búsquedas; la nuestra, de teoremas.

## Método (compartido por todos los R)

1. Doc de diseño por rework: física → política → predicciones falsables.
2. UNA rama coherente por rework, spins UCI por componente (0 = clásico
   bit-idéntico, verificado con bench).
3. Recibos ANTES de flota: los gates redefinidos del mate-lab (d10-d17,
   subset ≥20 ambos signos, fire-rate) + el oráculo atomicdb.
4. SPRT del PAQUETE con banda ancha ([0,5] — buscamos decenas, no décimas;
   un paquete que no resuelve rápido en banda ancha no es el rework que
   buscamos).
5. SPSA de las constantes nuevas SOLO tras pasar el paquete.
6. Destilación al net del árbol nuevo (el playbook del +55: la búsqueda
   nueva genera los datos que fijan la mejora en la eval).

## Orden de ejecución propuesto

1. **R-C autopsia** (T131: por qué la fundación cambió veredictos) — bloquea
   R-A/R-B; días.
2. **R-A** (MovePicker atómico) — el rework con la tesis más fuerte y el
   oráculo más directo.
3. **R-E fase oráculo** en paralelo (no compite por CPU de flota; es
   tooling + GPU).
4. **R-B** (podas) sobre el MovePicker nuevo — el orden correcto cambia qué
   podas disparan; secuencia, no paralelo.
5. R-E fase entrenamiento, R-D spell con el método probado.
