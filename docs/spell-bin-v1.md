# spell-bin v1 — formato normativo de datos de entrenamiento (76 bytes)

Estado: **NORMATIVO HISTÓRICO** para la serie run6. El formato no se ha mutado:
P2-a retiró su productor del motor y lo sustituyó por run7 v1 (44 bytes más
cabecera), definido por `tools/spellnnue-pytorch/run7.py` y documentado en el
README de ese directorio. `tools/psv_decode.py` sigue siendo el validador de
datasets run6 existentes.

El contrato de bytes está verificado contra el binario de tools de la referencia
FSF spell (AUDIT.md, fase 5): un registro nuestro y uno de la referencia para la
misma posición son byte-idénticos salvo el campo `move` (ver §3).

## 1. Registro `PackedSfenValue` — 76 bytes, little-endian

Los enteros multi-byte son little-endian (generado y consumido en x86-64; el
loader C++ del trainer mapea el struct tal cual — un host big-endian requeriría
swap explícito, no soportado en v1).

| Offset | Tamaño | Tipo | Campo | Semántica |
|---|---|---|---|---|
| 0 | 64 | `u8[64]` | `sfen` | Posición empaquetada, bitstream de 512 bits (§2) |
| 64 | 2 | `i16` | `score` | Eval de búsqueda en unidades internas, **POV del bando al mover**, clamp ±32000. Nunca scores de mate (la partida se corta antes, ver §4) |
| 66 | 2 | `u16` | `_pad` | Siempre 0 (alineación natural del `u32 move`) |
| 68 | 4 | `u32` | `move` | Primer movimiento del PV, codificación de 32 bits del motor (§3) |
| 72 | 2 | `u16` | `gamePly` | `Position::game_ply()` en el momento del registro (0 = posición inicial, blancas al mover) |
| 74 | 1 | `i8` | `gameResult` | Resultado final **POV del bando al mover en ESTE registro**: +1 acaba ganando, −1 pierde, 0 tablas |
| 75 | 1 | `u8` | `padding` | Siempre 0 |

`static_assert(sizeof(PackedSfenValue) == 76)` en `src/datagen.h`.

## 2. Bitstream `sfen` (512 bits, LSB-first)

Escritor: `Datagen::pack_sfen`. Los bits se escriben LSB-first dentro de cada
byte (`d[c>>3] |= 1 << (c&7)`); los campos multi-bit se emiten del bit menos al
más significativo. Orden exacto:

| # bits | Campo | Detalle |
|---|---|---|
| 1 | side to move | 0 = blancas, 1 = negras |
| 7 | rey blanco | índice de casilla 0-63 (A1=0, orden SF); **64 = rey capturado** (centinela fuera de tablero de la referencia) |
| 7 | rey negro | ídem |
| ~ | tablero | Escaneo fila 8→1, columna a→h, **saltando los reyes**. Casilla vacía = 1 bit `0`. Pieza = 5 bits `2*idx+1` (huffman LSB-first, idx: P=0 N=1 B=2 R=3 Q=4) + 1 bit color (1 = negra) |
| 2×8×5 | manos | Por color (blanco, negro), 8 slots de 5 bits en orden de piece-id FSF `P N B R Q K F J`; solo los slots 6 (Freeze) y 7 (Jump) son ≠ 0 |
| 2×2×24 | bloques de spell | Por color × {FREEZE, JUMP}: `has_zone` (1 bit), centro de zona (7 bits; 0 si no hay zona), cooldown (16 bits). El gate ES el centro de zona |
| 4 | enroques | KQkq en ese orden (`can_castle`) |
| 1(+7) | en passant | 1 bit presencia; si 1, 7 bits de casilla ep |
| 6 | rule50 bajo | bits 0-5 de `rule50_count()` |
| 16 | fullmove | `1 + (game_ply - (stm==BLACK))/2`, bits 0-7 y luego 8-15 |
| 1 | rule50 alto | bit 6 de rule50 |

El resto hasta 512 bits queda a 0. El tamaño de tablero es fijo 8×8 (la
referencia serializa files×ranks variables; aquí solo existe spell-chess).

## 3. Codificación del campo `move` (divergencia deliberada vs referencia)

`move = Move::raw()` del motor (`src/types.h`):

- bits 0-5: casilla destino · bits 6-11: origen · 12-13: pieza de promoción −2
  · 14-15: flag especial (1 promoción, 2 en passant, 3 enroque)
- bits 16-17: spell casteado con la jugada (0 ninguno, 1 freeze, 2 jump)
- bits 18-23: casilla del gate (solo significativa si 16-17 ≠ 0)

La referencia FSF usa su propia codificación de move de 32 bits (gating con
semántica FSF). **Mismo ancho, bits incompatibles**: un consumidor que mezcle
datasets de ambos productores debe decodificar `move` según el productor. El
trainer legacy solo consume `sfen`/`score`/`gameResult` (y `move` como filtro
opcional), por lo que la divergencia no afecta al entrenamiento run6.

## 4. Productor histórico retirado en P2-a — invariantes

`datagen out F count N nodes M [seed S] [random_plies R] [eval_limit E] [ply_limit P]`
(defaults: nodes 40000, random_plies 8, eval_limit 3000, ply_limit 400):

- Self-play a nodos fijos desde `StartFEN`; los primeros `random_plies` plies
  son aleatorios uniformes y **no** producen registros.
- Se registra una posición por ply buscado, ANTES de hacer la jugada.
- Terminales: rey capturado / sin jugadas legales (stall en jaque = derrota,
  stall quieto = tablas) / `is_draw` / score decisivo (|v| ≥ eval_limit o mate)
  — el score decisivo corta la partida y fija el resultado sin registrar esa
  posición, de modo que **ningún registro lleva score de mate**.
- `gameResult` se rellena al final de la partida con la paridad
  `(gamePly % 2)==0 ⇒ blancas al mover` (válido porque toda partida empieza en
  `StartFEN`, gamePly 0).
- Los registros se append-ean al fichero por partida completa (fichero abierto
  en modo append: varias corridas/procesos con seeds distintos concatenan sin
  cabecera — v1 no tiene magic ni manifiesto; eso llega con spell-bin-v2).

## 5. Compatibilidad con el pipeline

- Decoder/validador: `tools/psv_decode.py` (mismo contrato bit a bit).
- Loader del trainer: `training_data_loader.dll` (raíz de la organización) mapea
  el struct de 76 bytes directamente.
- Dataset canónico run6a: `spell-data/run6a/run6a_full.bin`
  (1.912.205 × 76 B); redes derivadas referenciadas por SHA-256 en AUDIT.md.
