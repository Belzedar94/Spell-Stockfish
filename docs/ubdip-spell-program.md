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
  no puede ejecutar la clavada y hoy cuenta como si pudiera.
  **Premisa corregida por sondas (3-ago): en spell chess NINGUNA clavada
  ata legalmente** — el auto-jaque es legal (se juega a capturar al rey) y
  `legal()` no filtra clavadas ni aquí ni en el oráculo FSF (tres posiciones
  de sonda, listas idénticas 10/10). `blockersForKing` es estructura de
  BÚSQUEDA (gives_check + filtro de clavadas de `see_ge`), así que el
  parche es perft-neutral por construcción. **HECHO**: rama
  `SB9-slider-blockers` (`9b27d068`), 34/34 tests, perft 61/61 vs oráculo,
  bench 15267414 con la red campeona, y recibos de `see`: clavada por
  puerta 0→208, clavadora congelada 208→0, y congelar la torre delantera
  no asciende a la trasera (las congeladas siguen BLOQUEANDO ocupación).
  Trampa evitada: limpiar snipers con `&~` y no `^` — un sniper sobre una
  puerta ya falta de la ocupación deslizante y el `^` lo re-añadiría.
- **EP+cast** (paridad de reglas, carril aparte del programa): chess.com
  permite poción + captura al paso en la misma jugada (verificado por
  rainrat, PR #6 de este repo); nuestro movegen las excluía en los dos
  brazos (`movegen.cpp:302/326`). Port a `nnue-v2` en curso; añade jugadas
  → cambian perft y bench, y va con SPRT [0,3]: si además da Elo, mejor,
  pero entra por corrección. Promociones siguen fuera también en la
  referencia (SPELL_SPEC tras el PR), así que ahí no hay deuda.

Ronda 2 = SB9 + EP-cast, prioridad 202: detrás de los LTC atómicos vivos
(T118/T150), delante del montón STC de 201. **Lanzados**: T152
`SB9-slider-blockers` (bench 15267414) vs SB2, STC [0,3] cap 20k.

**EP-cast entró por corrección, no por SPRT (3-ago, decisión del
propietario).** T151 se cerró sin jugar al caer en la cuenta de que era
incoherente por diseño: el runner relega jugadas como texto y la base sin
la regla no puede ni parsear `f@e7,e5d6` — habría medido desincronización,
no ajedrez. Regla general: **un cambio que amplía el movegen no se puede
A/B-testear por relay de texto contra una base sin el cambio**; o entra
por corrección con recibos (aquí: verificación de chess.com, 32/32 tests,
perft artesanal exacto, divergencias vs oráculo todas-ganancia), o se mide
por relay de FEN. **La base es ahora `15820b97`** (bench con red campeona
**17477858**); las ramas nuevas salen de ahí, y si T152 pasa, SB9 se
rebasa encima antes de mergear. El árbitro FSF local (build 11-jul) es
anterior a la regla: recompilar desde el PR #96 antes de cualquier panel
con EP-casts en juego.

## Torneo de redes FSF (3-ago): run5rl campeona

Formato semifinal→final del propietario, mismo motor a ambos lados
(`FSF_Spell_test_baseline`, solo cambia `EvalFile`), Hash=512, TCs de toda
la vida (2+0.02 / 10+0.1 / 30+0.3), `variantfishtest_spell.py`.

| Cruce | vstc | stc | ltc | Veredicto |
|---|---|---|---|---|
| 4rl vs 5rl (semi) | -53,8 (508p, LOS 0,0%) | -44,9 (140p, 5,5%) | 0,0 (66p) | **5rl** (adjudicado; dato histórico del propietario: ltc también 5rl) |
| 5rl vs 4b (final) | +40,3 (780p, LOS 100%) | +17,9 (252p, 79,8%) | +78,1 (86p, 98,5%) | **5rl** (adjudicado) |

Nota: invierte el orden de los torneos de rainrat a 300ms/mov (allí 4rl
arriba). A nuestros TCs, run5rl manda con claridad en vstc y ltc. **La
referencia FSF para paneles cruzados es `spell-chess_run5rl_e10_l07.nnue`**,
y el listón del programa es: Spell-SF (base EP-cast + red campeona propia)
debe batir a FSF+run5rl — si el generalista con red gana, el especializado
no se está justificando.

## El listón (3-ago) es INCOMPARABLE, no falso — y la red campeona es el problema

> **AVISO (5-ago): la tabla de abajo no mide lo que su título dice.** Mide
> **Spell-SF+HARD2** contra **FSF+run5rl** — o sea, motor Y red distintos a la
> vez. "Red campeona propia" es `spell-v2-HARD2` (`666BE56A`), la que fija el
> Protocolo de este mismo doc para ambos lados. Con la red igualada el signo
> se da la vuelta, y la propia red campeona resulta ser el eslabón flojo.
>
> **Batería A — Spell-SF+run5rl vs FSF+run5rl** (misma red en ambos lados):
>
> | TC | Partidas | W-L-D | Elo | LOS |
> |---|---|---|---|---|
> | 2+0,02 | 102 | 74-17-11 | **+219,3 ±76,9** | 100,0% |
> | 10+0,1 | 102 | 75-22-5 | **+200,1 ±78,5** | 100,0% |
> | 30+0,3 | 102 | 74-23-5 | **+190,9 ±77,3** | 100,0% |
>
> Gate del §7 cumplido: LOS 100% en los tres TCs con >100 partidas. **Con la
> misma red, el especializado gana al generalista por ~200 Elo a los tres
> TCs.** La premisa "si el generalista con red gana, el especializado no se
> está justificando" se cumple al revés de como se creía.
>
> **Batería B — Spell-SF+HARD2 vs Spell-SF+run5rl** (mismo binario, solo
> cambia `EvalFile`):
>
> | TC | Partidas | W-L-D | Elo | LOS |
> |---|---|---|---|---|
> | 2+0,02 | 116 | 41-75-0 | **−104,9 ±67,1** | 0,0% |
> | 10+0,1 | 444 | 189-247-8 | **−45,7 ±32,3** | 0,2% |
> | 30+0,3 | 148 | 75-73-0 | +4,7 ±56,4 | 56,5% |
>
> **HARD2 pierde contra run5rl dentro de nuestro propio motor**, con LOS
> cerrado en contra en VSTC y STC (444 partidas en STC) y empate técnico en
> LTC. Es la red que el Protocolo pone a ambos lados de todos los SB y la que
> lleva embebida el motor de OpenBench. Ojo al matiz: las dos redes entran por
> **rutas de eval distintas** — el banner dice `Spell NNUE loaded` con run5rl y
> `Spell NNUE v2 loaded` con HARD2 —, así que la batería B cruza red Y ruta de
> eval, no solo red. (Efecto colateral detectado: con la ruta v2 el comando
> `eval` no imprime `Final evaluation`; la búsqueda va bien, es solo la traza.)
>
> **Por qué la tabla vieja no se puede restar de la nueva.** Aritmética de la
> cadena a STC: Spell-SF+HARD2 vs FSF+run5rl ≈ (+200,1) + (−45,7) = **+154**,
> frente al −149 de la tabla. La red explica ~46-105 Elo; quedan ~300 sin
> explicar y **no se pueden atribuir**: el script del panel del 3-ago no está
> en el repo, ni en `Match script\`, ni en los labs de `D:`. Lo único que
> queda de aquello son los `panel_*.log` del 12-jul, que son otra medida.
>
> **Lo que sí se descartó, con recibos** (lab `Spell Project\chassis-gap-lab\`):
> binario distinto (`b6c3dd64` está a UN commit de `15820b97`; recompilado
> desde clon limpio da mismo perft y misma jugada a 1M nodos, y 34-5-1 vs FSF
> a 80k); binario de FSF distinto (el baseline de jul-11 y el PR96 juegan
> igual en 10 posiciones); red no cargada o perdida al re-enviar `UCI_Variant`
> tras `ucinewgame` (`tools\seq_vft.py`: sobrevive en los dos motores);
> coste fijo por jugada (0 ms en ambos); contabilidad de nodos; y el **relay
> de FEN**, reproducido con `tools\relay_pair_runner.py` (el runner de OB con
> un único cambio de contrato) contra su gemelo con historial, a la vez y al
> mismo TC: **no cambia el signo** (relay 15-6, historial 17-4), 0 posiciones
> repetidas 3+ veces en ninguna corrida, misma mezcla de desenlaces
> (adjudicación 12 vs 11, captura de rey 16 vs 19) y el relay deja el
> **106-108%** de los nodos por jugada, o sea que no cobra peaje de reloj.
>
> **Cadena de custodia auditada** antes de dar por bueno nada de esto: el
> binario moderno a solas reproduce jugada, score, profundidad y **contador de
> nodos exacto** (`f@d8,g4g5 / +0.46 / d11 / 200104`) de lo que el PGN
> atribuye a su lado; en las terminales por captura de rey gana siempre quien
> movió último; el recuento propio desde el PGN coincide con el marcador del
> runner en tres matches; y las baterías corren con `variantfishtest_spell.py`
> del propietario, cuya columna W es engine1 por construcción (verificado en
> el fuente, líneas 280-305), con `engine1 = spell-v2-base-b6c3dd64.exe` en la
> cabecera del log.
>
> **Consecuencia para el programa.** (1) "El hueco dominante de spell es de
> EVAL" se queda sin su recibo principal. (2) El Protocolo de más abajo fija
> HARD2 en ambos lados de todos los SB: eso no invalida los A/B (la red es
> común), pero significa que **se está tuneando la búsqueda sobre una red que
> pierde ~50-105 Elo contra otra que ya teníamos**, y que la ruta de eval v2
> merece una auditoría antes de seguir gastando SPRTs encima.
>
> Rutas de los seis logs:
> `Spell Project\chassis-gap-lab\gates\los-{ssf-run5rl-vs-fsf-run5rl,ssf-hard2-vs-ssf-run5rl}-{vstc,stc,ltc}\match.log`

<details>
<summary>Tabla original del 3-ago (retirada; se conserva por trazabilidad)</summary>

### El listón, medido (3-ago): FSF+run5rl nos saca 74-164 Elo

Cruce Spell-SF (base `15820b97` + red campeona propia) contra FSF+run5rl,
relay de FEN, árbitro `spell/nnue-potions`+EP compilado **`all=yes`**
(obligatorio: el gating puede pasar de 4.096 jugadas y el buffer estándar
corrompe el heap — 11.046 medidas por rainrat; nuestros dos primeros
intentos de panel murieron por eso con 59/200 y 39/200 abortos). Con
árbitro estable: **cero incidentes**.

| TC | Partidas | W-L-D | Score | Elo | LOS |
|---|---|---|---|---|---|
| 2+0.02 | 200 (final) | 50-138-12 | 28,0% | **-164** | 0,0% |
| 10+0.1 | 114 (parcial) | 32-78-4 | 29,8% | **-149** | 0,0% |
| 30+0.3 | 43 (parcial) | 17-26-0 | 39,5% | **-74** | 8,5% |

(Parciales: adjudicado por el propietario al quedar la dirección clara.)
Contra FSF **clásico** el mismo día: +123/+193 con LOS 100%. La lectura: la
brecha es la RED del generalista + su búsqueda tuneada para spell, no la
eval clásica; y se estrecha con el tiempo (patrón NNUE profunda). El
especializado aún no se justifica — de ahí la ronda 3.

</details>

## Ronda 3 (3-ago): la caza de los +100 — ordering e history

Tesis de ubdip para esta clase de juego (vault, 23-sep-25): en variantes de
branching enorme lo que paga es "better move ordering and history
heuristics". Su lista de atomic, traducida a spell, sobre la base
`15820b97`:

- **SB8 `frozen-history`** (item 3, el anillo→congelación): captureHistory
  gana dimensión = min(defensores congelados del destino, 3), 4 cubos, 4x
  memoria. EN CURSO.
- **SB7 `see0-tempo`** (item 4, la idea profunda): malus de orden TUNE
  (`SpellSee0Tempo`≈25) a capturas de SEE≈0 cuando el rival AÚN tiene cast
  (su recaptura puede venir con hechizo). Solo orden; poda en una segunda
  parametrización. EN CURSO.
- **SB5 `statscore`** (item 5): la captura SEE-perdedora (SEE de casts de
  SB2) deja de inyectar statScore positivo; `SpellStatScoreLosing` TUNE
  [-2048, 2048]. Fire-rate 15%; pin de neutralidad exacto. **T155**.
- **SB4 `frozen-grab`** (item 2): el diseño original ("defensa enemiga
  congelada en nuestro turno") resultó ESTRUCTURALMENTE MUERTO — la zona
  vive el ply del cast + UNA respuesta (`SPELL_ZONE_LIFETIME=2`; los "3
  turnos" son el cooldown), así que el bando sin turno nunca está
  congelado: 37,5M capturas medidas, 0 disparos. Es la lección de SB6
  (31-jul) golpeando por segunda vez: **la única congelación visible al
  puntuar una jugada es la que la propia jugada lanza**. Forma viva
  committeada: bonus `SpellFrozenGrab` a la captura freeze-gated cuya
  propia zona cubre a TODOS los defensores del destino (memoizado por
  destino, 63×; fire-rate 4,3%; cross-check con see_ge de SB2 en 5 casos).
  **T157**.
- **SB7 `see0-tempo`**: matiz del agente — el cooldown ticka en el do_move
  rival, el predicado honesto es `can_cast_on_reply` (cooldown ≤ 1).
  Fire-rate 3,4%; neutralidad exacta a 0. **T156**.
- **SB8 `frozen-history`**: **T154, EN CUARENTENA VERIFICATIVA** — la
  invariante de SB4 sugiere buckets casi-siempre-0 en las lecturas
  pre-move, pero su bench se movió (reproducido ×3); histograma de buckets
  en curso para resolver la contradicción antes de gastar flota.
- **T153 `capture-see-120b`** (recuperado por el inventario): el AUDIT.md
  registra `capture-see-120` como PASS STC contra la base vieja (10.308
  partidas, LLR +2,98, ~+10) con su LTC en cola cuando la era murió. SB2
  rehízo el SEE, así que revive como STC fresco de solo-opción
  (`SpellCaptureSeeMargin=120`, default 0) contra la base actual, ambos
  lados `15820b97`. Lanzado a 202.

**El inventario de experimentos (3-ago), corregido y ampliado:**
- Los `hito6_child_futility` de elo_logs son de ATOMIC, no de spell — esa
  cantera no existe. La autoritativa es **`AUDIT.md`** (1.067 líneas en
  este repo): ~25 veredictos SPRT de la era-torre, más el
  `perf_optimizations_log.txt` del fork FSF viejo.
- **Aviso de portabilidad**: v11 (orden por gate-impact) ganó **+100** en
  el fork FSF viejo y el mismo concepto midió **-676** en este chasis.
  Un ganador de un fork NO se presume ganador del otro; cada port se
  re-mide.
- Lista negra ampliada (veredictos-torre del AUDIT, NO re-testear tal
  cual): merged-ordering, razor-guard, spell-refutation, nullmove-guard,
  lmp-scale-200, conthist-skip, no-iir, futility-scale-150,
  aspiration-200, gatehist-off, pillar-A budget, volatility-scale,
  qsearch-spells, cast-decomposition-v2, MCTS, spell-see-ordering,
  spell-policy-v1/v2, spell-stages-v1; y no-penalty-pv pasó STC pero cayó
  en LTC (la ilusión clásica del TC corto).
- Del fork viejo quedan ganadores YA incorporados al linaje (movegen de
  freeze +45% NPS, reuso de jump, poda progresiva de puertas ~+80, lazy
  generation) — verificar que el chasis actual los conserva es parte de
  la higiene, no un test nuevo.

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
