# Spell-NNUE A ("SSNNa"): la arquitectura plana, sin king buckets

> Estado: implementada en el motor y en el entrenador, sin entrenar todavía.
> Formato ADICIONAL: SPL3 (v2) y el adaptador run5rl siguen cargando igual.
> Fichero de red objetivo: ~3 MB frente a los 101,8 MB de run5rl.

## 0. De dónde sale la idea

sscg13 (autor de las redes de shatranj, entrenadas con 10.000 millones de
posiciones) y Dean sostienen lo mismo: **los king buckets no se pagan solos
hasta volúmenes de datos que nosotros no vamos a tener**. Un bucket multiplica
por 32 el bloque de piezas; con 50M de posiciones viejas de d2 y 10M en
generación, cada bucket ve un 3% de los datos. Lo mismo, en peor, vale para
FullThreats: son 60.720 entradas x 1.024 = 62 millones de parámetros en un solo
bloque, diseñados para el corpus de SF (miles de millones de posiciones).

El branching factor de spell chess hace que ese corpus no exista ni vaya a
existir. La conclusión práctica: gastar los parámetros donde sí hay señal.

## 1. Qué cambia respecto a v2

| Bloque | v2 (`SpellKAv2`) | A (`SpellAv2`) |
|---|---|---|
| Piezas | 22.528 (32 king buckets, espejo a-h, reyes en un plano común) | **768** (12 planos de 64, sin bucket, **un plano por rey**) |
| Threats (`FullThreats`) | 60.720 | **fuera** |
| Zonas freeze | 4.096 (king-bucketeadas) | **128** (planas) |
| Zonas jump | 128 | 128 |
| Frozen | 128 | 128 |
| Globales spell | 30 | 30 |
| **Total por perspectiva** | **87.630** | **1.182** |

Todo lo demás es idéntico: L1=1024 con salida pairwise, 16 stacks
32/64/32/128, 16 buckets PSQT sobre la malla material x pociones,
cuantización y escalas de v2, termómetros de mano/cooldown con sus filas delta
precalculadas, y el bloque `frozen` explícito.

**Los reyes se separan de plano.** En HalfKA los dos reyes comparten
`PS_KING` porque el propio ya está codificado en el bucket. Sin bucket, un
plano compartido escondería cuál rey está dónde: por eso A usa 12 planos
(propias P N B R Q K, rivales P N B R Q K) en vez de 11.

## 2. Por qué también se van los threats

No es una segunda idea suelta, es la misma con la misma cuenta:

1. **Tamaño.** Los threats son el 90% del fichero (62 MB de i8). Sin tocarlos
   no se baja de 30 MB con ningún L1 razonable.
2. **Datos.** 62M de parámetros en un bloque para 60M de posiciones. Cada peso
   ve, de media, menos de una posición.
3. **Velocidad.** El bloque de threats es lo que domina el coste del
   acumulador (hasta 128 filas de 1.024 i8 por nodo) **y** es la única razón
   por la que en v2 un gate jump vivo fuerza refresco: al cambiar la ocupación
   de sliders, altera amenazas de piezas que la jugada no tocó.

Quitarlos deja una propiedad estructural fuerte: **ningún índice de `SpellAv2`
depende de una casilla de rey, así que ninguna jugada invalida una fila ya
acumulada**. `RequiresRefresh` es `false` en tiempo de compilación, la búsqueda
entera es incremental, y la tabla Finny pasa de `[64 casillas][2]` a `[2]`
entradas (de ~290 KB a ~4,5 KB por hilo).

## 3. Tamaño y velocidad medidos

Fichero (mismo chasis, redes aleatorias generadas por los `gen_random*`):

| Red | Bytes |
|---|---|
| run5rl (v1, FSF) | 101.788.576 |
| SPL3 v2 aleatoria | 92.281.360 (88,0 MiB) |
| **SPLA aleatoria** | **1.813.201** |

Una red A entrenada pesa algo más que la aleatoria (los pesos LEB128 pasan a 2
bytes): **~3,0 MB en crudo**, contra los ~99 MB de una v2 entrenada. Factor
~33x contra run5rl.

Velocidad, `bench` a 1 hilo en el mismo binario y la misma máquina (redes
aleatorias, así que los árboles difieren; el nps es la métrica comparable):

| Red cargada | nps |
|---|---|
| stock (sin red spell) | 447.379 |
| run5rl | 386.002 |
| SPL3 v2 aleatoria | 356.967 |
| **SPLA aleatoria** | **433.958** |

**+21,6% de nps sobre v2** y +12,4% sobre el adaptador run5rl.

## 4. Formato `SPLA`

Magic `0x53504C41`. El loader lo enruta antes que SPL3 y desplaza cualquier
otra red spell activa: un solo camino de evaluación a la vez.

```
u32 version = 0x53504C41
u32 net_hash          (ft_hash ^ arch_hash)
u32 desc_len, desc
u32 ft_hash           (0x4F234CB8 ^ (L1*2))
LEB128  biases   i16[1024]
LEB128  weights  i16[1182][1024]
LEB128  psqt     i32[1182][16]
16 x { u32 arch_hash, fc0/fc1/fc2 en crudo }   (idénticos a SPL2)
```

Los stacks son byte a byte los de SPL2, así que `spla.py` reutiliza los
helpers de `spl2.py`.

## 5. Lado del entrenador

Todo en `tools/spellnnue-pytorch/`, en paralelo a los módulos de v2 para no
tocar el camino que ya pasó su gate P1:

| Fichero | Papel |
|---|---|
| `features_a.py` | extracción `SpellAv2` en python puro; reutiliza `normalized_gates` y el bucket de salida de `features.py` |
| `spla.py` | escritor/lector SPLA y cadena de hashes |
| `model_a.py` | `SpellNNUEA` (una tabla de embeddings por cabeza, sin factorizador) y la referencia entera `quantized_forward` |
| `train_a.py` | driver de entrenamiento (mismo loss, lambda schedule y optimizadores que el de v2) |
| `serialize_a.py` | checkpoint `.pt` a `.nnue` SPLA |
| `gen_random_a.py` | red aleatoria estructuralmente válida para gates |
| `parity_a.py` | paridad motor vs python, con generador de posiciones sintéticas |

**No hay factorizador.** En v2 los 32 buckets de gates de freeze se
factorizaban a 64 filas virtuales; sin buckets no queda nada que factorizar.

## 6. Validación ya hecha

- Build MSYS2 `ARCH=x86-64-avx2 COMP=mingw`, sin warnings.
- `bench` con run5rl cargada por UCI: **4.958.980 nodos**, idéntico a main.
- Suite perft: **336/336**.
- Paridad `parity_a.py` sobre 1.000 posiciones sintéticas (426 con zona jump
  viva, 390 con zona freeze viva): **0 diferencias de features, 0 de eval,
  max diff 0 cp** contra la referencia entera de python.
- **Incremental vs refresco forzado**: con un parche temporal que refresca en
  cada nodo, `bench` con la red A da exactamente los mismos 8.863.608 nodos
  que el camino incremental. Es el gate de la maquinaria de diff: reproducible
  sustituyendo el cuerpo de `evaluate_a_side` por una llamada directa a
  `update_accumulator_refresh_cache_a`.

## 7. Plan de entrenamiento y test propuesto

1. **Datos**: los mismos que alimentarían a v2 (50M viejas de d2 + los 10M en
   generación, formato run7). A no cambia el formato de datos ni el generador.
2. **Overfit gate**: `train_a.py --records 1000000 --epochs 2 --start-lambda 1.0
   --end-lambda 1.0`. Debe converger igual que el gate P1 de v2.
3. **Paridad post-entreno**: `parity_a.py --net <red> --data <run7>` con al
   menos 1.000 posiciones reales antes de cualquier SPRT. Cero diferencias.
4. **Red 1**: corpus completo, `lambda` según la guía
   (`docs/nnue-training-guide.md`: lambda manda sobre épocas; arrancar en 1.0
   para bootstrap puro de eval).
5. **SPRT** contra el default actual (run5rl), STC y LTC, `[0, 5]` primero
   para detectar si la arquitectura es siquiera competitiva; si pasa, `[1, 6]`
   y panel formal de 3 TCs.
6. **Ablaciones, una variable por vez y solo después del primer pase**:
   - L1 1024 a 1536 o 2048 (sube el fichero a ~4,5 MB; exige bump de formato)
   - malla de buckets 16 a 8 (mat 4 x pociones 2) si los datos quedan cortos
   - devolver FullThreats sobre el feature set plano, para medir cuánto
     aportan de verdad a nuestra escala de datos

## 8. Riesgos conocidos

- **Puede quedarse corta de capacidad.** 1,2M de parámetros en el FT contra
  los ~49M de run5rl. Si la red 1 pierde por mucho, la primera palanca es L1,
  no volver a los buckets.
- **Sin threats, la red tiene que aprender la táctica desde los planos de
  pieza.** Es exactamente la hipótesis a falsar; el SPRT lo dirá.
- **La malla de 16 buckets divide los datos por 16** (riesgo ya anotado en
  `docs/spell-nnue-v2.md` §9). A no lo empeora, pero tampoco lo arregla.
- El bloque `frozen` sigue siendo explícito y el termómetro sigue siendo
  monótono: los dos aciertos de v2 se conservan intactos.
