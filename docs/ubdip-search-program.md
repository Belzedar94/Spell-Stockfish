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
- **Lanzados**: T159 `MV-R17-mcpbase-20` (solo-opción, prio 204);
  MV-R15 (jaques quietos en qsearch + TT de qsearch a dos niveles, el
  caveat del lab) y MV-R16 (movecount-skip en jaques quietos) en
  implementación con el **gate de crash-smoke** nuevo (80 partidas vs
  base, atribución por lado — nacido del incidente spell de hoy).
- Artefactos permanentes: `C:\mate-lab\matebench.py` + suite de 84; las
  ablaciones viven como spins UCI en el binario del lab (verificado
  nodo-a-nodo idéntico a la base con todo a 0).
- Idea de Wolfram aparcada con cariño: modo-solve del motor (comprometerse
  con una blanca fuerte y romperla; NNUE de solving separada) — tras la
  ola SPRT.

## Pendiente de responder a ubdip

Su pregunta de royalty ("more like mv-sf than fsf?"): nuestra
implementación es propia sobre el chasis SF-dev con FSF como oráculo de
reglas (perft-validada); reyes conectados/explosión implementados en
movegen+do_move nativos, no vía extinción genérica FSF.
