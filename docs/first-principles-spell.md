# Spell desde first principles: el programa de reworks

Compañero de `first-principles-atomic.md` bajo el mandato del 5-ago-2026
("pensar en grande"). Fuente de reglas: `SPELL_SPEC.md` (la única autoridad,
con el binario congelado de referencia y chess.com por encima de todo).

Contexto de fuerza (**revisado el 5-ago**): con la MISMA red en ambos lados,
Spell-SF+run5rl le gana a FSF+run5rl **+219 / +200 / +191** Elo con LOS 100%
a los tres TCs (102 partidas cada uno). El viejo listón de −74/−164 medía
otra cosa: Spell-SF con **HARD2** contra FSF con run5rl, y HARD2 pierde
~50-105 Elo contra run5rl dentro de nuestro propio motor. Ver
`ubdip-spell-program.md`. El renglón "el hueco dominante de spell es de
EVAL" se queda sin recibo y hay que re-derivar a dónde va el presupuesto;
datagen run9 → run1c y la minería siguen en marcha mientras tanto. Pero la ronda 3 de knobs de
búsqueda (SB4-SB9) está igual de neutra que la atomic, por la misma razón
estructural: son trocitos sobre formas clásicas. Este doc hace el ejercicio
completo para cuando el método (probado en atomic) llegue aquí — y una parte
(S-A, el picker de puertas) no necesita esperar: es donde la búsqueda de
spell se juega su identidad.

## 0. La física de spell

1. **La explosión combinatoria es LA física.** Startpos: **1814 legales**
   (20 normales + 1124 freeze-gated + 670 jump-gated) — ~91× el ajedrez.
   (Corregido el 5-ago: decía 1878 = 20+1188+670. Recontado con `go perft 1`
   sobre el startpos en los DOS binarios actuales — Spell-SF `b6c3dd64` y
   `FSF_Spell_PR96_f3c0d204_allvars` — y los dos dan 1814 con el mismo
   desglose; el 1188 de freeze estaba 64 alto.)
   Medio juego cargado: hasta 8-32k pseudo-legales (types.h dimensiona los
   buffers a 32k). Cada jugada base existe además en ~decenas de copias
   gated (`f@gate,move` / `j@gate,move`). El motor ya rinde el universo en
   la BÚSQUEDA con topes fijos de puertas (`MaxFreezePotionGates = 12`,
   `MaxJumpPotionGates = 6`, overrides de anillo de rey; el universo legal
   completo solo para perft/validación/evasiones/defensa urgente). Ese
   pre-picker artesanal es la decisión de búsqueda MÁS importante del motor
   y hoy es una lista fija.
2. **El self-check es legal; ganar es capturar el rey.** No hay filtro de
   legalidad por jaque (excepción única: el rey no puede MOVERSE a casilla
   atacada; sub-excepción: sí puede si captura al rey enemigo — fin
   inmediato). "Estar en jaque" = el rival amenaza capturar tu rey en una:
   una restricción BLANDA — ignorable por contraataque más rápido. Las
   evasiones no son forzadas; los cross-checks y las carreras de reyes son
   tácticas de primera clase.
3. **Freeze = arma de tempo y anti-defensa.** Zona 3×3 en cualquier casilla
   (5 usos, cooldown 3 propias). Congela ORÍGENES enemigos durante
   exactamente el ply del cast + LA respuesta rival; y — crítico — **las
   piezas congeladas NO ATACAN**: no dan jaque, NO DEFIENDEN, no impiden
   enroques. Congela-al-defensor-y-captura (la tesis de SB4) es el patrón
   táctico nativo; congela-al-jaqueador convierte no-evasiones en
   continuaciones legales (el movegen ya lo genera en EVASIONS).
4. **Jump = arma de geometría.** Una casilla OCUPADA se vuelve transparente
   para los sliders de AMBOS bandos (2 usos, cooldown 3): descubiertas a
   demanda, "snipers" a través de la puerta contra el rey
   (`spell_jump_snipers`), clavadas que aparecen y se esfuman, y el
   phase-flip de los peones (empujes que aterrizan "capturando" hacia
   delante en la transparente ocupada). Ambos bandos la usan: castearla es
   prestársela al rival durante su réplica.
5. **Los hechizos son una ECONOMÍA.** 5F+2J por bando, cooldown 3, y las
   manos/cooldowns están en la red (pocket + planos de zona/cooldown) y en
   el Zobrist. Castear es semi-irreversible: gasta un recurso finito con
   valor de fase (un freeze en el final vale otra cosa que en la apertura).
   El árbol debe tratar el cast como decisión de INVERSIÓN, no como jugada
   más.
6. **Los efectos son ultra-transitorios.** Zona viva = ply del cast + una
   réplica; entre-movimientos solo puede existir UNA zona activa (invariante
   comunitario del spec). Consecuencia sutil: las estadísticas de búsqueda
   (history, killers, counters) indexadas por jugada MEZCLAN contextos con
   y sin zona que no se parecen en nada — y la MISMA base move existe en
   ~60 identidades gated distintas que o se conflan (butterfly clásico) o
   se mueren de hambre (indexar por gate).

## 1. Auditoría de supuestos clásicos

| Maquinaria | Asume (clásico) | Realidad spell |
|---|---|---|
| MovePicker por etapas | ~35 jugadas; capturas ≈ táctica | 1800+; la táctica nativa es cast+quiet (freeze-grab) y el 97% del universo son casts casi todos irrelevantes |
| Topes de puertas fijos (12F/6J) | — (no existe en clásico) | ES el picker de verdad y no aprende: ni history de puertas, ni presupuesto por posición, ni precio de economía |
| History/killers butterfly | una identidad por from-to | 60 identidades gated por base move: confla o hambruna; el contexto de zona (transitorio) no existe en el índice |
| LMR/LMP por índice de jugada | "tardía" ≈ mala | el índice crudo a 1800 ramas no significa nada; un cast probado el 200º no es "tardío" semánticamente |
| SEE / defendida | defensas estables | una "defendida" es grabable vía freeze-al-defensor si hay F en mano; una clavada es rompible vía jump; SEE sobreconfía en defensas condicionales al estado de manos |
| Evasiones forzadas | jaque restringe legalidad | self-check legal: evasión = urgencia heurística; contraataque más rápido es respuesta válida |
| Null move | pasar es seguro salvo zugzwang | regalar un ply a quien tiene casts en mano es regalar cast+move; y "en jaque no null" cambia de sentido con el jaque blando |
| Repetición/TT | estado = tablero | ya resuelto: zonas/cooldowns/manos en Zobrist (spec §2) — la base está bien puesta |

## 2. Los reworks

### S-A. El picker de puertas aprendido (el rework identitario)

**Tesis**: los topes fijos 12F/6J son un MovePicker artesanal congelado en
el sitio más caliente del motor. La política nativa:

1. **History de puertas**: tablas por (spell, gate, contexto-grueso) —
   contexto mínimo: cuadrante relativo al rey enemigo/propio y fase — que
   aprenden QUÉ puertas pagan, como el butterfly aprende from-to. Alimenta
   la selección top-K en vez de la heurística fija actual.
2. **Factorización cast = base ⊕ puerta**: score(gated) = history(base) +
   gateHistory(spell, gate, ctx) + prior económico. Ni conflar ni
   hambrear: las dos mitades generalizan por separado. (SB8b/cast-history
   era el embrión knob de esto; T158 −0,24 dice que la mitad sola no paga.)
3. **Prior de economía**: precio de mano decreciente (el 5º freeze barato,
   el 1º caro), descuento por cooldown de oportunidad (castear ahora =
   no poder en 3), bonus urgencia (anillo propio amenazado + F en mano).
4. **K adaptativo**: presupuesto de puertas por nodo función de
   profundidad, historia del nodo (¿los casts han sido bestMove aquí?) y
   urgencia — en vez de 12/6 constantes universales.
5. Etapas: TT → táctica dura (capturas ganadoras) → **casts top-K por el
   score factorizado** → quiets → casts resto (bajo demanda, lazy) →
   perdedoras. La generación de casts se vuelve PEREZOSA por tramos (hoy el
   coste de generar 1800 jugadas se paga aunque el nodo corte en la 3ª).

**Recibos**: fire-rate de casts como bestMove por etapa/puerta; rango medio
del cast-bestMove antes/después; nodos por decisión en suite de tácticas
freeze-grab (minable de las partidas ya jugadas: posiciones donde el
ganador casteó y el eval saltó); bench d10-d17; y paridad EXACTA del
universo legal (perft intacto — esto solo toca orden y generación perezosa,
jamás legalidad).

### S-B. Podas para 1800 ramas

- **Reducción por CLASE y rango-en-clase**, no por índice crudo: un cast
  K-ésimo de su etapa se reduce por (clase cast, rango K), un quiet por su
  historia — el moveCount global deja de significar.
- **Cast-futility**: un freeze cuya zona no toca ninguna pieza enemiga
  relevante (ni atacantes de nuestro anillo, ni defensores de nuestros
  targets, ni el jaqueador) se poda barato sin buscarlo; espejo para jumps
  cuya transparencia no abre línea útil (sniper-check del spec como
  predicado). Contadores de disparo obligatorios.
- **Null move condicionado a manos**: margen/profundidad del null
  endurecidos cuando el rival tiene F/J en mano y cooldown 0 (su "mejor
  respuesta al pass" incluye un cast); relajados con manos vacías (ahí el
  juego es ajedrez).
- **Urgencia como estado**: el análogo del in-check clásico es "rey propio
  capturable en una" (jaque blando) — puertas de poda cerradas ahí, con la
  generación de defensa urgente que el movegen ya distingue.

### S-C. Amenaza condicional a manos (SEE/defensas honestas)

**Tesis**: "defendida" es una mentira estadística cuando el atacante tiene
freeze en mano (congela al defensor: la defensa no existe durante su ply) y
"clavada" lo es con jump (la transparencia des-clava). Sistema: descuento de
defensas/clavadas condicionado a (mano rival, cooldown, distancia de zona) en
los DOS consumidores — qsearch stand-pat/margen y orden de capturas. SB9
(slider-blockers, +0,60 vivo) y SB7 (see0-tempo) son embriones knob de esto;
la versión sistema decide con el estado de economía, no con constantes.

### S-D. El jaque blando como búsqueda nativa

Extensiones/urgencia re-derivadas para 2: amenaza-de-captura-de-rey como
estado graduado (¿cuántos atacantes, cuántos defensores post-freeze?), 
cross-checks y carreras (mi mate en N vs su mate en N−1) como patrón de
primera clase en qsearch — hoy qsearch hereda la lógica clásica de jaques
duros sobre un juego donde el jaque es una amenaza negociable.

### S-E. Eval y datos (el frente dominante, ya en marcha)

- datagen run9 (50M, en flota) → run1c: el camino corto del listón.
- Minería r2 (lateral, diminishing returns aceptados): fase 1 en curso.
- **Curriculum de economía**: los datos de self-play parten del startpos
  con manos llenas; el espacio (manos × cooldowns × fases) está
  sub-muestreado en colas (finales con 1F vs 0). random_plies ya castea
  uniforme; medir la cobertura de manos en el dataset y, si está sesgada,
  sembrar aperturas de datagen con estados de mano diversos.
- La red ya VE la economía (pockets + planos zona/cooldown, contrato §6):
  el techo es de datos, no de features.

## 3. Método y recibos (compartido con atomic, adaptado)

Spell no tiene el oráculo de verdad probada de atomicdb; sus recibos:
paridad perft/universo EXACTA en cada rework (la legalidad jamás se toca),
bench d10-d17 para coste, suites minadas de las propias partidas (tácticas
freeze-grab, jump-snipers, defensas urgentes) con time-to-move del binario
de referencia FSF como vara, y el protocolo de fuerza del spec §7: VSTC →
STC/LTC, LOS 100% a 3 TCs >100 partidas, sin excepciones — el listón
−74/−164 se re-mide por TC tras cada paquete.

## 4. Orden

1. **S-A** en cuanto el método quede probado en atomic R-A (mismo
   implementador de plantilla) — o antes si la ronda 3 muere entera: es
   independiente del resultado atomic.
2. S-B sobre S-A (la reducción por clase necesita las clases del picker).
3. S-C como paquete propio (sus embriones SB9/SB7 habrán dado veredicto).
4. S-D tras S-A/S-B (qsearch nuevo sobre orden nuevo).
5. S-E corre SIEMPRE en paralelo — es otro presupuesto (GPU/flota de
   datagen) y es el frente que mueve el listón hoy.
