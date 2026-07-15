# Spell-NNUE v2 ("SSNNv2") — diseño desde first principles

> Estado: DISEÑO aprobado-pendiente (2026-07-14). Rompe compatibilidad con todo
> lo anterior deliberadamente. Sustituirá tanto la red stock de SF (ajedrez
> puro) como el adaptador `src/spellnnue/` (run5rl, arquitectura FSF 2021).

## 0. Puntos de partida (medidos, no recordados)

**Chasis actual (SF master 2026, en nuestro repo, `src/nnue/`):**

| Componente | Valor exacto |
|---|---|
| Feature sets | HalfKAv2_hm (22.528; 32 king-buckets, espejo a↔h) + FullThreats (60.720; sin bucket) = 83.248 inputs |
| Acumulador | L1=1024 por perspectiva (i16), salida por **multiplicación pairwise** acc[j]·acc[j+512] → u8 |
| Stacks | 8 (selección `(pieceCount−1)/4`): 1024→32 (sparse i8) → {sqr,lin}CReLU concat 64 → 32 → {sqr,lin} concat 128 → 1, + skip fc_0[30]−fc_0[31] |
| PSQT | 8 buckets i32 en el FT, `(psqt[stm]−psqt[~stm])/2` |
| Update | DirtyPiece (refresh solo al mover el rey, Finny tables por [ksq][color]) + DirtyThreat (lista u32 empaquetada, cota 96) |
| Fichero | 90,6 MB; Version 0x6A448AFA; LEB128 salvo threatWeights (i8 raw) |

**Autopsia v1 (FSF `spell/nnue-potions`, red run5rl 101,8 MB):**

- Base FSF = SFNNv2 (~2021): 512×2 → 16 → 32 → 1. Dos generaciones por detrás
  del chasis actual (sin threats, sin pairwise, sin sparse-input, L1 mitad).
- Aciertos: zonas como planos casilla incrementales (castear NO refresca),
  manos como pockets termómetro.
- Pecados: **cooldown 0..3 en binario sobre 16 bits** (56/64 features muertos,
  encoding no monótono — cooldown 1=`01` y 2=`10` no comparten ningún bit);
  **globales multiplicados por 64 king-buckets** (el cooldown no depende de
  dónde esté el rey: ×64 de inputs sin señal); **frozen implícito** (la red
  debe aprender el AND zona∩pieza); MaxActiveDimensions subido a ojo.

**Estado spell a representar (spec §estado):** manos 5F+2J por bando;
cooldown 0..3 por (color, spell); una zona activa por (color, spell) — gate
de freeze (mancha 3×3 recortada) y gate de jump (1 casilla transparente);
piezas congeladas = piezas propias dentro de zona freeze rival viva. Todo ya
está en el Zobrist y en `StateInfo` → el diff incremental es natural.

## 1. Principios de diseño

1. **El chasis moderno se conserva; solo cambia QUÉ ve.** FT + pairwise +
   stacks sparse + cuantización + Finny son ingeniería de una década que no
   vamos a redescubrir. Los cambios son: feature set, ejes de bucket, y la
   maquinaria de diff para eventos spell.
2. **Globales planos, tablero bucketeado.** Lo king-relativo se multiplica
   por king-buckets; lo global (manos, cooldowns) jamás (pecado v1 #2).
3. **Ordinales pequeños = termómetro** (sscg13, 2026-07-14; v1 ya lo hacía en
   manos y no en cooldowns): al variar el conteo en ±1 cambia EXACTAMENTE un
   feature → update incremental mínimo y monotonicidad aprendible.
4. **Lo táctico explícito, no derivable.** El FT es lineal: no puede hacer
   AND entre planos. Si "pieza congelada" importa (importa: es la mecánica
   central del juego), es un feature, no una inferencia.
5. **No pagar por lo que el chasis ya regala.** ~~FullThreats ya ve los
   ataques a través de gates~~ — **CORREGIDO en P0 (verificación de Codex,
   2026-07-15)**: la enumeración de threats del chasis usa ocupación stock,
   NO los attack maps spell-aware; la paridad 1000/1000 lo prueba. Hacer los
   threats spell-aware (que un slider "amenace a través" de un gate vivo) es
   un EXPERIMENTO FUTURO propio, con su SPRT — no un regalo ya cobrado.

## 2. Feature set `SpellKAv2` (por perspectiva)

| Bloque | Inputs | Activos típ. | Bucketeo | Nota |
|---|---|---|---|---|
| HalfKAv2_hm | 22.528 | ≤32 | 32 kb | sin cambios |
| FullThreats | 60.720 | variable | — | sin cambios; spell-aware vía attack maps |
| **Zonas freeze** (gate propio + rival) | 2×64×32 = 4.096 | ≤2 | **32 kb** | king-bucketeado: la denegación de casillas de fuga es un objeto de seguridad del rey |
| **Zonas jump** (gate propio + rival) | 2×64 = 128 | ≤2 | plano | los efectos de línea ya los llevan los threats; el gate plano cubre el resto (p.ej. doble paso desbloqueado) |
| **Frozen** (pieza congelada en casilla, propio/rival) | 2×64 = 128 | ≤9 | plano | el AND zona∩pieza, explícito (principio 4) |
| **Globales spell** | 30 | ≤30 | plano | ver desglose |
| **Total** | **87.630** | | | +4.382 sobre stock (+5,3%); ~+9 MB de pesos |

**Globales spell (30):** manos termómetro F propio ≥1..≥5 (5) + J propio ≥1,≥2
(2) + rival (7) = 14; cooldown termómetro ≥1,≥2,≥3 × {F,J} × {propio,rival}
= 12; **ready-bits** explícitos (mano>0 ∧ cooldown==0) × 4 = 4. Los ready-bits
violan a sabiendas "no derivable" en la otra dirección: son EL bit táctico
("puede castear YA") y como AND no lo aprende el FT lineal, se regala.

**Índices** (dentro del bloque HalfKA extendido): tras los 22.528 de piezas
vienen [zonas freeze 4.096][zonas jump 128][frozen 128][globales 30]. El plano
de color es relativo a la perspectiva (como v1); orientación de casillas de
gates/frozen con el mismo `OrientTBL ^ flip` de HalfKA para las bucketeadas y
solo `flip` vertical para las planas.

**Qué se rechazó y por qué:**
- *Mancha 3×3 como plano* (v1): con el gate king-bucketeado + frozen explícito
  la mancha es derivable y solo infla activos (9 vs 1 por zona).
- *Globales × king-bucket* (v1): sin señal, ×32 de coste.
- *Plano de transparencia jump en piezas* ("este alfil ataca a través"):
  duplicaría FullThreats.
- *Features relativos al gate* (geometría gate-rey): el king-bucketeo de los
  gates de freeze ya da esa resolución; el resto es sparsity sin datos.
- *Piezas frozen como tipos de pieza nuevos* (frozen-P, frozen-N...): duplica
  el espacio HalfKA (+22k inputs) y obliga a refresh de 9 piezas por cast en
  el plano principal; el plano frozen aparte da lo mismo por 128 inputs.

## 3. Buckets de salida: el eje de fase de pociones

Sustituimos `bucket = (pieceCount−1)/4` (8) por una malla 2D **material ×
pociones**:

```
mat    = min(3, (pieceCount - 1) / 8)          // 4 niveles
potion = min(3, potionsTotalAmbos / 4)         // 14..0 → 4 fases
stack  = mat * 4 + potion                      // 16 stacks, 16 PSQT buckets
```

Racional: la fase de spell chess la define el agotamiento de recursos tanto
como el material — con manos cargadas la eval es dominada por seguridad del
rey (3% de tablas, mates tempranos); sin pociones ES ajedrez. El PSQT
bucketeado por fase captura "cuánto vale una dama cuando quedan 7 pociones
enemigas vs 0" — exactamente la clase de término que el SPSA de fase 4 dejó
entrever. Coste: los stacks son diminutos (fc_0 1024×32 i8 = 32 KB); 16
stacks ≈ 0,5 MB. PSQT pasa de 8 a 16 columnas i32 (+0,7 MB).

## 4. Maquinaria incremental

- **`DirtySpell`** (nuevo, análogo a DirtyThreat): lista de u32 empaquetados
  `(add|remove, bloque, índice)` que `do_move` rellena en los puntos donde ya
  muta spell state: cast (mano −1 = 1 flip termómetro; cooldown 0→3 = 3 flips
  + ready-bit; gate add; frozen add ≤9), tick de cooldown en el do_move del
  rival (1 flip ± ready ± gate-expiry + frozen removes ≤9), y entrada/salida
  de piezas de una zona viva por movimiento normal (≤2 flips frozen).
  **Peor caso real** (cast de freeze sobre 9 piezas con expiry simultáneo del
  rival): 1+3+1+1+9+9+2+2 = 28 flips spell — cota dura, documentada, y
  `MaxActiveDimensions` se deriva de ella (nada de "a ojo", pecado v1 #4).
- **requires_refresh sin cambios** (solo movimientos del propio rey). Los
  gates de freeze king-bucketeados se refrescan con el rey vía Finny — la
  `AccumulatorCaches::Entry` se extiende con los 4 gates + bitboard frozen +
  contadores (manos/cooldowns) para que el diff del cache incluya spell.
- Los threats siguen su camino actual sin tocar.

## 5. Cuantización y formato

- Tipos/escalas idénticos a stock (FT i16, spell block i16 como HalfKA;
  stacks i8/i32; mismas saturaciones). Los features spell son 0/1 como los
  demás → sin re-derivación de rangos.
- **Version nuevo: `0x53504C32`** ("SPL2"). El hash-chain de SF ya compone
  dimensiones → cualquier desajuste red/binario se rechaza en load. Ficheros
  `spell-v2-<sha12>.nnue`, embebido por INCBIN como stock. `src/spellnnue/`
  (adaptador run5rl) se retira el día que v2 pase SPRT — un solo camino de
  eval.

## 6. Entrenador ("spellnnue-pytorch")

- Fork del nnue-pytorch actual (ya entrena HalfKAv2_hm+FullThreats): se añade
  `SpellKAv2` al dataloader C++ (compila con nuestro MSYS2; entrena en la
  3080/cu118 — el mismo stack del policy head).
- **Factorización** (solo en train): los 32 buckets de gates de freeze se
  factorizan a un plano virtual de 64 (como hace el factorizer de HalfKA con
  el rey); frozen-plane se factoriza contra el plano de pieza base (una pieza
  congelada = pieza + delta frozen). Mejora generalización con datos escasos.
- Loss estándar SF (interpolación eval/WDL con lambda-schedule). Posiciones
  terminales por captura de rey etiquetan mate (nuestro universo permite
  auto-jaque: la red DEBE ver posiciones con rey en prise y evaluarlas).
- Validación de paridad: export → load en el motor → eval bit-exacta vs
  referencia python en 1k posiciones antes de cualquier SPRT.

## 7. Datos (run7)

- Generador: self-play del propio motor (reglas corregidas post-936ab213),
  nodos fijos ~40k, aperturas del libro spell + temperatura primeros plies,
  filtros del generador de ubdip adaptados (skip posiciones con best=captura
  o en jaque para el target de eval), rescoring a profundidad fija.
- Registro: struct binario propio con el estado spell completo (el FEN
  extendido ya lo serializa; el packer lo comprime a ~40 B/posición). binpack
  de SF se descarta de momento: su delta-encoding asume universo de jugadas
  de ajedrez; extenderlo es optimización prematura a nuestra escala.
- Volumen: bootstrap 50M → net-1; iteración 300M con self-play de net-1.
  La granja genera de noche (CPU); el entreno no compite con los SPRT (GPU).

## 8. Fases y gates (cada una falsable)

| Fase | Entregable | Gate |
|---|---|---|
| P0 (3-5 d) | `SpellKAv2` + buckets 16 + DirtySpell + serialización SPL2 en el motor, tras knob | bench sin red v2 idéntico; con red random: no crash, perft/suite intactos |
| P1 (~1 sem) | trainer + dataloader + paridad | eval bit-exacta motor↔python; overfit de 1M posiciones converge |
| P2 (3-6 d compute) | run7 (50M bootstrap) | distribución de fases/manos auditada (nada de 90% manos llenas) |
| P3 (1-2 d/red) | net-1 → SPRT STC/LTC [1,6] vs default actual | pasa → default + panel formal 3-TC vs oráculo re-medido |
| P4 | iteración datos/ablaciones (L1 1536, malla de buckets, frozen on/off) | solo tras el primer pase; una variable por vez |

## 9. Riesgos conocidos

- **Datos mandan**: la arquitectura perfecta con 50M posiciones mediocres
  pierde contra run5rl con sus 600M+. El plan de datos es tan crítico como el
  feature set; por eso P2 tiene gate de auditoría propio.
- El eje de buckets por pociones divide los datos por 16: si P2 queda corto,
  arrancar con 8 (mat 4 × potion 2) y crecer.
- La cota de 28 flips por cast es ~10× un quiet normal: si el profiling de P0
  muestra >2% de coste en update, degradar frozen-plane a recompute-on-access
  (está acotado a 18 activos).
