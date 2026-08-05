# Reworks desde first principles (índice)

Mandato del 5-ago-2026: "hay grandes partes del motor como pruning, see,
move ordering, etc. que están optimizadas para ajedrez clásico; no vamos a
conseguir cientos de elo solo cambiando pequeños trocitos, necesitas pensar
en grande". La evidencia: 13 SPRTs de knobs cerrados en neutro el mismo día
(§ elo-strategy-2026-08.md) y el mate-lab demostrando que un bonus sobre la
estructura clásica no compra la brecha.

El programa vive en dos documentos, uno por motor, cada uno con su física,
su auditoría de supuestos clásicos, sus reworks como paquetes coherentes y
sus recibos:

- **`first-principles-atomic.md`** — capturar es explotar, el intercambio
  igualado pierde tempo, reyes conectados crean inmunidad. Reworks: R-A
  MovePicker atómico, R-B podas conscientes de la explosión, R-C blast_see
  como único oráculo (autopsia T131 bloqueante, en curso), R-E la red
  entrenada contra la verdad probada de atomicdb.
- **`first-principles-spell.md`** — 1878 legales en startpos y el picker de
  puertas fijo como decisión más caliente del motor, self-check legal,
  freeze que apaga defensas, jump que abre geometría, los hechizos como
  economía. Reworks: S-A picker de puertas aprendido, S-B podas para 1800
  ramas, S-C amenaza condicional a manos, S-D jaque blando nativo, S-E
  eval/datos (el frente dominante del listón −74/−164).

Método común a todos: doc de diseño → una rama con spins de ablación
bit-idénticos a 0 → recibos antes de flota (gates redefinidos del mate-lab;
oráculo atomicdb en atomic, suites minadas + referencia FSF en spell) →
SPRT del paquete en banda ancha [0,5] → SPSA después → destilación al final
(el playbook del +55). Los detalles y el orden de ejecución, en cada doc.
