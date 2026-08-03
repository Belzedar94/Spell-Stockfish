# Programa SB: heurísticas de búsqueda de spell chess (30-jul-2026)

El mismo ejercicio que el programa UB de atomic, aplicado a Spell-Stockfish.
Origen doble: el feedback de ubdip sobre Atomic-Stockfish (traducible casi
línea a línea porque el diagnóstico es estructural, no atómico) y el torneo
de rainrat del 30-jul, donde Spell-Stockfish con la red `666BE56A` hace
40/80 contra los 62,5/80 de run4b — es decir, **perdemos contra FSF tuneado
con nuestra propia red**. Si la evaluación es la misma, lo que falta está
en la búsqueda.

## El diagnóstico traducido

El de ubdip para atomic: *el fork conserva las heurísticas de captura del
ajedrez estándar, que en atomic son semánticamente incorrectas*. En spell
chess el error equivalente no está en el valor de las capturas —capturar
sigue ganando la pieza— sino en **quién ataca qué**: freeze quita atacantes
y jump los añade a través de las puertas. La primitiva que decide podas,
orden y qsearch es el SEE, y ahí es donde el fork sigue siendo genérico.

## La rama correcta (trampa que casi nos cuesta la noche)

El motor que OpenBench testea NO es `master`: es la rama **`nnue-v2`**
(tip `ddd49df5`), la única que lleva el feature set SpellKAv2 y puede
cargar la red campeona `spell-v2-HARD2.nnue` (`666BE56A`). `master` es un
linaje viejo — le faltan `src/nnue/spell_v2.*` y `spell_ka_v2.*`, unas
4.000 líneas — y una idea medida ahí no dice nada sobre el motor real.
Comprobado: `src/` de `4c7e8d13` (el motor 64 de OpenBench, base de T85/T89)
es **idéntico** al del tip de `nnue-v2`, así que el bench validado por los
workers, **16284926**, sigue siendo el de la base. Toda rama SB sale de ahí
y cualquier build limpio de la base sin parche debe dar ese número exacto.

## Inventario (recibos, rutas sobre `nnue-v2`)

| Sitio | Estado | Item ubdip |
|---|---|---|
| `position.cpp:811-818 attackers_to` | YA spell-aware (transparencia de gates, exclusión de congeladas) | base |
| `position.cpp:2116/2132/2141 see_ge` | **el conjunto inicial es spell-aware, los rayos X del bucle NO** | 4 |
| `position.cpp:2064` | **SOLO los casts con base CASTLING** salen por `type_of() != NORMAL` → SEE = 0; los de base NORMAL sí entran al bucle, pero evaluando con las zonas ACTUALES e ignorando la que el cast crea (`make_spell` escribe en los bits 16+, `type_of()` lee los 14-15) | 4 |
| `position.cpp:777 update_slider_blockers` | snipers/pinners/blockers con `pieces()` crudo: sin transparencia de gates ni exclusión de congeladas, y `see_ge` consume esos pinners en su filtro de clavadas | 4 (rama propia) |
| `search.cpp:1959` | futility de qsearch suma `PieceValue[pieza en to_sq]` | 1 |
| `search.cpp:1488` | `statScore = 809*PieceValue[captured]/128 + captHist` | 10 |
| `movepick.cpp:259` | orden de capturas = `captHist + 7*PieceValue[captured]` | 2 |
| `movepick.cpp:276` | penalización "amenazado por pieza menor" con PieceValue | 2 |
| `captureHistory[pc][to][capturedType]` | sin datos de hechizo | 3 |
| `GateHistory` | pesos a 0 por SPSA-2 (el ordering aprendido resultó ruido) | — |

**El hallazgo que manda el programa:** dentro de `see_ge`, el conjunto de
atacantes inicial se calcula con `attackers_to`, que es spell-aware; pero
los rayos X que el bucle descubre al retirar piezas usan `occupied` crudo.
Dos errores en direcciones opuestas: un rayo X **no** ve a través de una
puerta de jump (debería) y **sí** incluye piezas congeladas (no deberían
poder recapturar). Todo lo que consume SEE hereda ese ruido.

## Escalera de parches (independientes, cada uno su rama y su SPRT STC)

Disciplina de ubdip del 30-jul: **una idea por diff, ortogonales, nada
acumulado**. Cada rama sale de la misma base, no se apilan.

- **SB1 `see_ge` consistente**: los rayos X del bucle usan la misma máscara
  que `attackers_to` (`occupied & ~jump_transparent()`) y excluyen
  `frozen_pieces()`. Corrección de coherencia, no una heurística nueva.
- **SB2 SEE de casts**: un cast deja de valer 0 por definición; se evalúa
  el movimiento base del cast con la maquinaria de SB1, más el efecto del
  hechizo sobre los atacantes de la casilla destino.
- **SB3 futility de qsearch spell-aware**: `search.cpp:1906` sustituye el
  valor crudo de la pieza en `to_sq` por el delta que ya sabe calcular SB1.
- **SB4 orden de capturas**: `movepick.cpp:258` pasa de
  `7*PieceValue[captured]` a un término que respeta si la defensa está
  congelada o si la puerta abre una recaptura invisible.
- **SB5 statScore**: `search.cpp:1440`, el mismo cambio que U4 en atomic.
- **SB6 amenazas sin sentido spell** (item 2b): test de ELIMINACIÓN del
  bonus/malus de amenazado-por-menor cuando el amenazante está congelado.
- **SB7 malus de tempo** (item 9): un cast invierte un tempo y el rival
  responde dentro de la zona viva; una captura SEE≈0 con cast disponible
  del rival es peor de lo que dice el SEE.
- **SB8 history con estado de hechizo** (item 3): dimensión de cooldown /
  congelación en `captureHistory`, al estilo del anillo de explosión.

## Estado (31-jul, mañana): la base se movió

**SB2 pasó STC (+74 Elo) y LTC (+85 Elo, LOS 100%) y ES la base**:
`nnue-v2` tip `2817f9e7`, bench **14594664**, motor 64→**107** en OpenBench.
Las ideas vivas se rebasaron encima y se relanzaron; los tests contra la
base vieja (T138/T139/T140) quedan cerrados y no se heredan.

| Test | Rama | Bench | Idea |
|---|---|---|---|
| T145 | `SB1b-see-consistency` | 15353002 | rayos X del bucle con reglas de spell (ahora también en la rama Cast) |
| T146 | `SB2c-cast-charge` | 14594664 + opción | el SEE de un cast cobra la carga que gasta (`SpellCastSeeCharge=60`) |
| T147 | `SB6b-frozen-threats` | 16996504 | amenazas congeladas, sobre la base nueva |
| T148 | `SB3b-qs-futility` | 15936070 | futility de qsearch, sobre la base nueva |
| T149 | `SB2b-cast-xray` | 16301497 | transparencia del cast en todo el intercambio — **cotas [-3, 1]** |

**Por qué T149 va con cotas de no-regresión**: su propia instrumentación
dice que voltea **28** veredictos de `see_ge` por bench, frente a los
**695.614** que volteó SB2. Ratio 24.800:1. Es corrección del modelo, no
palanca de Elo, y con [0,3] habría sido un empate larguísimo.

**Dónde está el volumen** (mismos contadores): el 93,7% de las llamadas a
`see_ge` son jugadas con hechizo y el 97,7% de los casts que llegan son
FREEZES, no jumps. O sea que la superficie gorda la tocan SB1b y el item
pendiente de `update_slider_blockers` — no las variaciones sobre jump.

**Trampa de alta**: un test de solo-opción (knob TUNE con default 0) debe
registrar el bench de opciones POR DEFECTO, que será idéntico al de la base;
el worker corre `bench` a secas y un bench medido con el knob activado da
"Wrong Bench" y cierra el test con 0 partidas (pasó con T146).

## Estado (3-ago): cierre de la ronda 1

La avería del worker local (m12, `-T 24` → `bad_alloc` → la base regalaba
partidas) invalidó la primera pasada; con el cliente 42 desplegado los
relanzamientos midieron limpio, y dijeron ruido:

| Test | Rama | Partidas | LLR | Cierre |
|---|---|---|---|---|
| T145 | `SB1b-see-consistency` | 1.536 | -0,23 | archivado |
| T146 | `SB2c-cast-charge` | 1.802 | -0,44 | archivado |
| T147 | `SB6b-frozen-threats` | 1.536 | -0,06 | archivado |
| T148 | `SB3b-qs-futility` | 1.536 | -0,08 | archivado |
| T149 | `SB2b-cast-xray` | 0 | — | cerrado sin jugar |

Doctrina cumplida: cada familia tuvo sus dos parametrizaciones (la original,
que en las ventanas limpias de la flota sana midió 49-50%, y la variante
b/c sobre la base nueva). T149 se cierra sin jugar porque su premisa ya
estaba medida — 28 vuelcos de veredicto por bench, 24.800:1 contra SB2, y
su prima SB1b en ruido — y con una flota de 2 máquinas un test de
no-regresión no paga su asiento.

**Post-mortem de la ronda.** SB2 ganó +74/+85 porque corrigió QUÉ CUESTA un
cast: verdad de primer orden, 695.614 veredictos volteados por bench. Los
refinamientos de la estructura fina del bucle de intercambio (rayos X,
recapturadoras congeladas, márgenes de qsearch, filtros de amenaza) midieron
todos ruido a STC — ahí no vive el Elo de spell a estos controles. Lo que
queda de primer orden y sin tocar:

- **SB9 `update_slider_blockers`** (`position.cpp:777`): snipers/pinners
  con `pieces()` crudo. Una torre enemiga detrás de una puerta de jump
  clava A TRAVÉS de la puerta (la transparencia vale para deslizadoras de
  ambos bandos) y hoy esa clavada es invisible; una "clavadora" congelada
  no puede ejecutar la clavada y hoy cuenta como si pudiera. Lo consumen
  `legal()` y el filtro de clavadas de `see_ge`. Es la misma clase de
  corrección que SB2, no un refinamiento del bucle.
- **EP+cast** (paridad de reglas, carril aparte del programa): chess.com
  permite poción + captura al paso en la misma jugada (verificado por
  rainrat, PR #6 de este repo); nuestro movegen las excluía en los dos
  brazos (`movegen.cpp:302/326`). Port a `nnue-v2` en curso; añade jugadas
  → cambian perft y bench, y va con SPRT [0,3]: si además da Elo, mejor,
  pero entra por corrección. Promociones siguen fuera también en la
  referencia (SPELL_SPEC tras el PR), así que ahí no hay deuda.

Ronda 2 = SB9 + EP-cast, prioridad 202: detrás de los LTC atómicos vivos
(T118/T150), delante del montón STC de 201.

## Estado anterior (31-jul, madrugada) — contra la base vieja

| Test | Rama | Bench | Qué mide |
|---|---|---|---|
| T138 | `SB6-frozen-threats` | 14588885 | un cast de freeze silencia las amenazas que su propia zona cubre |
| T139 | `SB3-qs-futility` | 15943085 | el margen de qsearch pregunta si la casilla está defendida DE VERDAD |
| T140 | `SB1-see-consistency` | 15652404 | los rayos X del bucle de intercambio obedecen las reglas de spell |
| T141 | `SB2-cast-see` | 14594664 | el SEE de un cast ve la zona que el propio cast crea — **PASÓ STC** (LLR 3,06 en 1.712 partidas, 1020-659-33; penta 121/10/407/23/295 cuadrado). LTC de confirmación: **T143** |

Los tres reprodujeron la base en **16284926** antes de tocar nada (el
cortafuegos: si un toolchain no reproduce la base, su bench bloquearía a
toda la flota). Perft intacto en los tres. Prioridad 201, compartiendo
flota con la oleada atomic.

Hallazgo de SB6 que merece recordarse: el agujero literal de `attacks_by()`
(no filtra congeladas) **no puede dispararse en juego** — la vida de la zona
garantiza que el bando al que le toca mover nunca tiene una zona propia
activa — y se demostró con bench bit-idéntico. La única congelación que un
quiet puede ver mientras se le puntúa es la que él mismo lanza.

## Lista negra (ya refutado, NO re-testear)

- Ordering estático por gate-impact en MovePicker: **-676**.
- `GateHistory` aprendido: SPSA-2 llevó sus dos pesos a 0.
- C9, "solo hechizos tácticos en el horizonte": **-20,8**.
- QSPELL (hechizos en qsearch): **-163** a nodos fijos.
- Limitar movimientos base: **-200**.

## Protocolo

Base: rama `nnue-v2` (motor 64 de OpenBench, bench **16284926**) con la red
`spell-v2-HARD2` (`666BE56A`) en AMBOS lados. Ramas `SB<n>-<slug>`
independientes desde esa base, nunca apiladas. Bench limpio por rama
(receta `ob-test-cloning`; el archive del motor en este repo apunta al
COMMIT, no al tree — distinto de Atomic). Opciones `Threads=1 Hash=32`,
libro `spell_openings.epd`. SPRT STC 8+0.08 [0.00, 3.00] cap 20k; el que
pase, LTC 40+0.4 [0.00, 2.50] cap 12k contra la base vigente. Nada de
one-shots: dos o tres parametrizaciones con post-mortem escrito antes de
archivar una idea.
