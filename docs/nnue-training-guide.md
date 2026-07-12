# Guía de entrenamiento NNUE para variantes (destilada del Discord)

Fuente: mensajes de **Belzedar** (el propietario) en el Discord de Fairy-Stockfish,
2021-2022, extraídos del archivo local `org/vault/vault.sqlite` (canales help,
nnue-training, development, janggi, xiangqi, test-results). Fechas citadas para
trazabilidad. Aplica a redes de variantes entrenadas con (variant-)nnue-pytorch.

## La regla de oro: lambda >> épocas

> "The difference between epochs won't be big normally, while playing with Lambda can
> give you better results when finding the right one" (2022-07-04)

- **Método**: entrenar el MISMO dataset varias veces con lambdas distintos y elegir por
  match/LOS. Barridos típicos: cada 0.10 (0.05/0.15/0.25...) o grueso (0/0.33/0.66/1),
  luego refinar (2022-07-04, 2022-05-31).
- `lambda = peso de la eval` (1.0 = solo evals; 0 = solo resultado de partida)
  (2022-05-26).

## Lambda por etapa y por variante

- **Etapa temprana de una red fuerte → más peso al resultado**: "in the first stages of
  training a strong net you need to give more weight to the result of the game (I use
  lambda 0.8). When the RL progress is plateauing, then you can increase the lambda"
  (2022-04-19). "As the net learns, usually you need to increase lambda" (2022-07-06).
- **Resultados empíricos por variante** (el óptimo es MUY dependiente de la variante):
  - Atomic: mejor 0.15-0.25 — "Statistically stronger than any Lambda >0.5" (2022-06-17,
    2022-07-14). Relevante para variantes tácticas/decisivas (spell probablemente aquí).
  - Ataxx: 0.8-0.9 en fase madura (2022-05-31); con 20M d5 tempranos, 0.3 rindió 123 elo
    igual que 0.9 con 55M (2022-05-25).
  - Grasshopper: 0.45 (2022-06-09) · Xiangqi: 0.75 (2022-07-22) · Janggi: 0.8 (2022-07-14).
- Lambda 0 puro = ignora evals → con él, la profundidad del datagen da igual (2022-08-10);
  suele rendir mal en solitario (-418 elo vs -368 de λ0.9 en un caso temprano, 2022-05-23).

## Épocas

- Epoch del trainer = `--epoch-size` posiciones (default 20M en nuestro fork).
- Con datasets ~100M d5: `--max_epochs 9` en el loop RL, tomando la ÚLTIMA época
  (2022-03-31); los mejores checkpoints solían caer entre e5 y e13 — "for 100M I would
  expect epoch 5-6 to be the best, but 13 is rocking" (2022-03-23).
- Si dudas: convertir varias épocas (1/3/5/...) y testearlas, o simplemente la última
  (2022-03-24, 2022-03-28).

## La receta RL completa (plantilla extinction, 2022-03-31)

```
datagen:  generate_training_data depth 5 count 100000000 random_multi_pv 4
          random_multi_pv_diff 100 random_move_count 8 random_move_max_ply 20
          write_min_ply 5 eval_limit 10000 eval_diff_limit 500 data_format bin
          (con la red del run anterior cargada, Use NNUE = pure)
train:    python train.py DATA DATA --lambda <λ> --max_epochs 9 --threads 4
          --num-workers 8 --gpus 1  (+ --resume-from-model <run-anterior>.pt)
serialize: python serialize.py logs/.../last.ckpt <variant>_run<i>rl.nnue
loop:     regenerar 100M con la red nueva y repetir; el nombre runNrl = iteración N
```
(run5rl de spell = quinta iteración de este loop.)

Hiperparámetros base que usaba como referencia del trainer estándar SF (2022-03-20):
batch 16384, random-fen-skipping 3, gamma 0.995, lr 4.375e-4.

## Aplicación a Spell-Stockfish run6a (2026-07-12)

Dataset propio de 1.912.205 posiciones a 5000 nodos (E2E; dataset serio vendrá después).
Decisión conforme a la guía: mini-barrido λ ∈ {0.25, 0.75, 1.0} × 4 épocas (epoch-size
20M), elegir por match a nodos fijos vs run5rl sobre nuestro motor. Etapa temprana +
variante decisiva → esperable que gane un λ medio-bajo. Nota: para lambda<1 el campo
`result` del PSV es imprescindible (nuestro datagen lo rellena por backfill ✅).
