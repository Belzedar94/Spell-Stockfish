# Pilar B — Descomposición del cast (diseño de implementación)

Objetivo: convertir el ply gated `(cast s@g, move m)` — hoy una arista opaca de
un espacio ~1650 — en DOS medias decisiones consecutivas del MISMO bando:
`cast s@g` (nodo intermedio "pending") seguido de `m`. b≈1650 → b≈165+35.
Precedente: motores de Arimaa (branching ~17k) descomponen el turno en pasos.
Todo detrás de un toggle `SpellDecompose` (0 = camino clásico, bench-idéntico).

## Invariante maestro (el gate de corrección)

Para toda posición P, gate g y jugada base m legal en el universo gated:
`P.do_cast(s,g); P.do_move(m)` debe producir EXACTAMENTE el mismo estado
(key incluida) que `P.do_move(gated(s,g,m))`. Test: **perft de equivalencia**
(perft clásico == perft descompuesto contando solo plies completos) sobre la
suite de 61 posiciones, d1-d3. Sin esto verde no se toca search.

## Pasos

### B1. Position: estado pendiente (1 sesión) — DISEÑO REFINADO 2026-07-13
Hallazgo (do_move:1309-1348): el bloque spell de do_move fusiona cast propio +
limpieza de zonas caducas + tick del rival. NO se extrae: el pending se vuelve
**declarativo**:
- `Position` (NO StateInfo): campos `pendingSpell/pendingGate` + accessors.
- `do_cast(s,g)` = set pending + nada más (O(1), sin StateInfo nuevo);
  `undo_cast()` = clear. `Position::key()` devuelve
  `st->key ^ pendingKey[s][g]` cuando hay pending (tabla Zobrist nueva).
- `do_move(m)` con pending: compone `m2 = m.raw() | spellbits(s,g)`, limpia el
  pending y ejecuta el do_move CLÁSICO con m2 → el estado final es
  byte-idéntico por construcción (mismo código, invariante estructural).
- Movegen del pendiente NO necesita estado aplicado: la restricción freeze
  sobre orígenes propios es estática (FreezeBlockBB[g]) y la transparencia
  jump ya se computa stateless por-gate en generate_spell_moves; la zona solo
  afecta al RIVAL, que juega después de completar el ply.
- Repeticiones/is_draw: sin StateInfo pending no entran nunca ✓. Eval/TT del
  nodo pendiente usan key() con pendingKey ✓. undo_move(m2) clásico ✓.

### B2. Movegen del nodo pendiente (1 sesión)
- `generate<PENDING_MOVES>`: jugadas base legales bajo la restricción del gate
  propio (`FreezeBlockBB[gate]` para freeze: el caster no puede salir del gate
  ni sus ortogonales — movegen.cpp:295) + para jump, las jugadas de slider
  habilitadas por la transparencia del gate (lógica ya existente en
  generate_spell_moves refactorizada por-gate).
- Movegen de casts en el nodo normal: emitir "pure cast" como Move sentinel:
  bits de spell (tipo+gate) con base `from==to==gate` (from==to es ilegal en
  jugadas reales → sentinel seguro; cuidado con Move::none()/null()).
- Perft de equivalencia (B1+B2 juntos): herramienta `perft decomposed` o test
  python dedicado.

### B3. Integración en search (1-2 sesiones)
- Toggle `SpellDecompose`: la etapa SPELL del MovePicker emite CASTS (~130)
  en vez del producto (~1650); un cast al hacerse (do_cast) lleva a un nodo
  pendiente donde el MISMO bando busca `generate<PENDING_MOVES>` (~35).
- Nodo pendiente: MISMO alpha/beta, MISMO color (max-max, sin cambio de
  signo), sin stand-pat, sin eval estática (assert !pending en evaluate),
  sin null move, sin razoring/futility de nodo (es un medio-nodo), TT con la
  pendingKey (depth semantics: el cast consume 0 plies? propuesta: el nodo
  pendiente hereda depth-0.5 redondeado — empezar con MISMO depth y LMR
  aplicado solo a la mitad move).
- Ordering: casts por gate impact score (existente) + historia propia de
  casts (geometría relativa al rey — la señal validada por SPSA) como
  seguimiento; la mitad move usa el stack normal (history/LMR/LMP en espacio
  ~35 ✓ el objetivo del pilar).
- ss->currentMove/continuation history en el pendiente: usar el cast sentinel
  (piece = NO_PIECE? definir moved_piece(cast) = caster? v1: saltar updates
  de conthist en la frontera cast, como ContHistSkip).
- Time management/mate scores: plies REALES no cambian (game_ply no avanza
  en el pendiente) ✓ mate distances intactas.

### B4. Endurecimiento y SPRT (1 sesión)
- Bench idéntico con toggle off; suite completa; sanitizers CI; xboard
  intacto (los protocolos solo ven jugadas completas SIEMPRE).
- SPRT STC→LTC con SpellDecompose=1. Si pasa: los presupuestos del pilar A se
  re-expresan naturalmente (LMP normal sobre casts) y GateHistory renace como
  historia del medio-move cast.

## Riesgos señalados
- Divergencia do_cast+do_move vs do_move(gated): mitigado por helper único +
  perft equivalencia (EL gate).
- TT: cutoffs de nodos pending con bounds heredados — verificar que un bound
  almacenado en pendingKey nunca se lee desde una posición real (keys
  disjuntas por tabla Zobrist propia ✓).
- Singular/extensiones en la frontera: v1 los desactiva en nodos pending.
- MultiPV/root: la raíz NUNCA es pending (los casts de raíz se expanden
  completos como hoy — decomposición solo en interior) → UCI intacto.

## Estado
- 2026-07-13: diseño escrito. B1 empezado en rama `cast-decomposition`.
