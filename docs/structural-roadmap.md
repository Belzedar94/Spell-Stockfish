# Roadmap estructural (2026-07-13) — pensar el motor PARA spell chess

Encargo del propietario: dejar de limar diferencias con la baseline y rediseñar
desde first principles. Sin código todavía: diagnóstico + plan.

## 0. El diagnóstico que lo reordena todo (medido hoy)

A 10 s fijos por posición (movetime, 1 hilo):

| Posición | Spell-SF | FSF baseline |
|---|---|---|
| startpos | depth **2**, 2.211 nodos | depth **8**, 407.166 nodos |
| book con zona activa | depth 2, 3.379 | depth 8, 95.009 |
| midgame manos llenas | depth 2, 12.734 | depth 8, 357.696 |

Completamos depth 2 en ~50-130 ms y **no cerramos depth 3 en 10 s**. Cuadra con
la evidencia acumulada: a nodos fijos estamos a −17 (por nodo somos ≥ iguales),
el bench a profundidad limitada da 462k NPS (1,76× la baseline), y aún así
−146/−111/−55 por tiempo con el gap encogiéndose al crecer el TC (más tiempo →
más cerca de completar iteraciones). No hay time losses (paneles: 0).

Causa de diseño (lectura de código, pendiente de perfilado formal): la etapa
SPELL ignora `skipQuiets` por diseño y **ninguna poda por conteo (LMP) corta los
spells**; en cada nodo interior se visitan los ~300 gated moves supervivientes
del cap de gates (8F+4J × jugadas), cada uno reducido por LMR hasta qsearch.
Coste por nodo interior ≈ 300 qsearches ⇒ la iteración d→d+1 multiplica por
cientos, no por ~2-3. Las 12 hipótesis-toggle refutadas eran ajustes DENTRO del
paradigma "visitar todos los spells"; el problema es el paradigma.

## 1. Pilar A — Disciplina de iteración: presupuesto de spells por profundidad
(progressive widening; 2-4 semanas de trabajo incremental, el primer gran salto)

En árboles de branching enorme (Go pre-AZ, Arimaa, MCTS) la técnica estándar es
**ensanchar progresivamente**: a profundidad d solo se consideran los f(d)
mejores hijos "extra", con f creciente. Traducción a nuestro AB:

- Presupuesto de spells por nodo dependiente de depth (y de mejora/PV/estado
  táctico): p.ej. d≤2 → solo spells tácticos (clasificador existente); d=3 → 4
  mejores gates; d=5 → 8; PV/inCheck → más. El resto NI SE GENERA (el cap de
  gates ya es infra para esto: hacerlo función de depth en vez de constante).
- Los spells entran en el conteo de LMP con peso propio (hoy invisibles).
- Gate de aceptación: depth alcanzada a 10 s comparable a FSF (≥7) ANTES de
  medir Elo; después SPRT normal. Métrica nueva de primera clase: "profundidad
  a tiempo fijo" en 3 posiciones canónicas, en cada iteración del molino
  (lección del agente Atomic: los fracasos ruidosos son de time-shape, no de
  fuerza — vigilar time losses).
- Riesgo: perder tactos de spell profundos → mitigado porque el clasificador
  táctico ya existe y el widening crece con depth.

## 2. Pilar B — Descomposición del cast (búsqueda factorizada estilo Arimaa;
el rediseño profundo, 1-2 meses, tras validar A)

El espacio de acción es factorizable: (castear spell en gate) × (jugada). Hoy
cada combinación es una arista opaca (~1650 en midgame). Descomponer el ply en
dos medias decisiones — nodo "cast pendiente" (mismo bando, sin cambio de
signo) seguido de la jugada — convierte b≈1650 en b≈165+35:

- Alpha-beta rinde b^d: mismo presupuesto ⇒ mucha más profundidad efectiva.
- Ordering/history/LMR/LMP operan sobre niveles de ~200, no 1650; las
  estadísticas del cast (cuándo castear, geometría relativa al rey — la señal
  que el SPSA validó) tienen su sitio natural en la media-decisión de cast.
- Precedente directo: los motores de Arimaa (branching ~17k) descomponen el
  turno de 4 pasos en pasos individuales con estado "pasos restantes".
- Coste/riesgos: clave TT para el estado intermedio, invariantes de repetición,
  NNUE no se evalúa en nodos intermedios, cuidado con dobles extensiones. Es
  EL cambio grande y largo; A nos da Elo mientras B madura, y B hereda la
  infraestructura de presupuesto de A.

## 3. Pilar C — Quiescence consciente de spells (el horizonte, tras A)

El recurso más violento del juego (freeze) es "quiet": la qsearch no lo ve y
los stand-pat son optimistas contra amenaza de cast. Con 3% de tablas, los
errores de horizonte deciden partidas.

- Fase 1: en qsearch, generar un puñado acotado de casts FORZANTES (freeze que
  cubre al defensor de una captura; freeze/jump que habilita captura de
  material colgado o del rey) — "pseudo-capturas" con SEE-de-spell (ganancia
  habilitada − coste tempo/recurso). Presupuesto durísimo (≤2-4 casts/nodo qs).
- Fase 2 (dual): margen de amenaza-de-cast en stand-pat cuando el RIVAL puede
  castear (reutilizando freeze_gate_score desde su perspectiva, solo en hojas).
- Riesgo conocido: explosión de qsearch — por eso va DESPUÉS del régimen de
  presupuestos de A.

## 4. Pilar D — Selectividad escalada por volatilidad del estado de spells

La única idea ACEPTADA del agente FSF-spell ("risk-aware selective search":
reducciones/podas escaladas por potions/cooldown/volatilidad) apunta igual que
nuestro hallazgo LMR-cap (el descuento por moveCount se desboca). Versión
nuestra: un multiplicador de selectividad por nodo función de (manos, cooldowns,
zona activa, |eval| swing) aplicado a LMR/futility/razoring como GRUPO — un
mecanismo, no 10 toggles. SPSA-able con ~6 parámetros.

## 5. Lo que NO haremos (evidencia en contra)

- PUCT/policy en raíz (2 fracasos del agente FSF: inestabilidad y time losses).
- Interleave/merged ordering (fracasó en ambos motores).
- Escudos anti-poda por amenaza (fracaso −500 con time losses).
- Nuevas redes/arquitecturas NNUE: expresamente pospuesto por el propietario.

## 6. Orden propuesto y gates

1. **A0 (1 día)**: instrumentar — contadores de spells visitados/nodo por depth,
   perfil de la iteración d3 en startpos, verificación de dónde aplican los
   depth penalties (¿raíz?). Confirmar el mecanismo exacto de la explosión.
2. **A (semanas)**: presupuesto por depth → gate "depth≥7 a 10s" → SPRT STC/LTC.
3. **D (días, en paralelo con A)**: multiplicador de volatilidad, SPSA corto.
4. **C fase 1** tras A; **B** como proyecto mayor una vez A/C estabilicen.
5. Re-medir el panel formal tras cada pilar; releases según escalera del PLAN.

Cada pilar es falsable por SPRT y ninguno depende de red nueva. El objetivo del
conjunto no es cerrar −55: es que "profundidad a tiempo real" deje de ser
nuestro cuello de botella y el resto del stack de SF master pueda por fin pagar.
