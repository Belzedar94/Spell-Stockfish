# Tres apuestas grandes (2026-07-13, noche) — más allá de los pilares

Encargo: tres proyectos del calibre del MCTS/descomposición, capaces de dar
saltos reales de fuerza en Spell. Cada uno con su porqué, su plan y su gate.

## Apuesta 1 — Spell-SEE: evaluador estático de intercambio PARA CASTS

**Idea.** El SEE clásico responde "¿qué gano capturando aquí?" con lógica de
intercambio sin búsqueda. Spell chess no tiene el equivalente para su recurso
central: "¿qué gano casteando aquí?". Hoy lo aproximamos con bonos planos
(zona-material, rey/anillo) que el SPSA apenas rescata. Un Spell-SEE real:

```
see_freeze(g) = max sobre jugadas propias m habilitadas/protegidas por la zona:
    ganancia_de_intercambio(m | defensores dentro de la zona NO cuentan)
  - mejor réplica estática del rival fuera de la zona (aprox: su mejor
    captura/counter no congelada, misma lógica de intercambio)
see_jump(g)   = reveal exacto (ya lo tenemos) extendido con el intercambio
    en la casilla revelada (no solo "ve al rey/material": ¿gana el cambio?)
```

Todo con bitboards y la maquinaria see_ge existente (ocupancias modificadas:
`occ & ~zona` para defensores congelados). Sin búsqueda, O(piezas en zona).

**Dónde paga.** Un número (cp) por cast que alimenta a la vez: (a) ordering
de la etapa SPELL y de los centinelas del pilar B, (b) la qsearch del pilar C
v2 (gate por see_freeze > umbral en vez de clasificador binario), (c) márgenes
de futility/poda para casts (un cast con see≈0 se poda como quiet basura),
(d) priors del MCTS. Un mecanismo, cuatro consumidores.

**Gate.** Bench-idéntico off; SPRT del ordering primero (sustituir KingBonus/
RingBonus por see_freeze en freeze_gate_score); después los consumidores uno
a uno. Riesgo conocido: coste por nodo (medir NPS; cachear por (zobrist de
zona-relevante) si hace falta).

## Apuesta 2 — Política aprendida para casts (spell-policy, red MINÚSCULA)

**Idea.** Todos los caminos (MCTS sin red, gate history muerto por SPSA,
refutation table neutra) apuntan al mismo agujero: no tenemos P(cast | posición).
La solución con techo real es aprenderla — pero NO hace falta una NNUE grande:
una cabeza de política de ~10-30k parámetros sobre features baratas
(geometría gate↔reyes, material en zona, manos/cooldowns, fase, spell-SEE de
la apuesta 1) que produzca P(castear), P(tipo), P(gate | tipo).

**Los datos YA existen**: cada registro PSV lleva el move del PV (76 bytes,
campo `move` — 1,9M ejemplos en run6a, y el datagen serio multiplicará eso).
Etiqueta = ¿el PV castea, qué tipo, qué gate? Entrenamiento: minutos de CPU.
Inferencia: un producto escalar por gate candidato — despreciable en NPS.

**Dónde paga.** Priors del MCTS (su mayor debilidad hoy), ordering/widening de
la etapa SPELL clásica y de los centinelas B, y el presupuesto del pilar A
(gastar el budget según P en vez de score estático).

**Gate.** Extraer el dataset de política de run6a (script, sin tocar motor) →
entrenar la cabeza → validar AUC/top-k offline → integrar tras aprobación del
propietario (moratoria de redes nuevas: esto es una red de POLÍTICA, no de
eval — pedir luz verde explícita antes de entrenar).

## Apuesta 3 — Pilar B fase 2: TT pendiente + historia geométrica de casts

**Idea.** La descomposición (d13-14 vs d16-18 del clásico) pierde por tres
carencias mecánicas, todas resolubles:
1. **TT en nodos pendientes**: la pendingKey ya existe (overlay Zobrist);
   probar/almacenar bounds en el medio-nodo convierte las transposiciones de
   cast (¡masivas: gates equivalentes, órdenes cast/move!) en cutoffs gratis.
   El dedup de sscg13 es un caso particular que la TT generaliza.
2. **Historia geométrica del cast**: [tipo][bucket geométrico gate↔rey rival]
   [¿zona toca al rey propio?] — la señal king-relative que el SPSA validó,
   como historia APRENDIDA del medio-move cast (ordering de centinelas +
   widening). Sustituye al gate-history absoluto muerto.
3. **Completions por generación staged** (hoy: filtro sobre MoveList<LEGAL>,
   O(universo) por centinela): emitir directamente los base moves legales
   bajo el gate declarado (FreezeBlockBB / transparencia por-gate ya
   existen) — coste por centinela O(35).

**Gate.** Cada pieza es un commit bench-idéntico off + SPRT del toggle B
actualizado. Meta intermedia medible: decompose ≥ profundidad clásica a 10s
en las 3 posiciones canónicas (con pipes drenados…). Si se alcanza, la
cobertura extra de casts decide el SPRT.

## Orden sugerido

Spell-SEE primero (desbloquea a los otros dos y no necesita permisos),
B-fase-2 después (infra ya caliente), política aprendida cuando el
propietario levante la mano con las redes. Los tres son falsables por SPRT
y ninguno toca la eval.
