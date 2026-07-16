# Programa de experimentos: staging de spells v2 (respuesta al feedback de ubdip 2026-07-14 20:24)

## El diagnóstico que corrige el método

ubdip, literal: el intento v1 "se rindió demasiado pronto sin considerar en serio
los ANDs de las condiciones; parece muy asustado de falsos negativos de spells
buenos". Ejemplo suyo: freeze al rey SIN que el rey esté en jaque (directo o vía
jump disponible) = inútil salvo escenarios raros — y nuestro is_tactical_spell
lo clasifica como táctico incondicionalmente. El clasificador es un OR amplio
(miedo a perder spells buenos) → la clase "táctica" es enorme → el staging no
mordió → −77 y autopsia equivocada ("la estructura está bien, la frontera mal"
era verdad a medias: la frontera no se EXPLORÓ).

Regla nueva de método (aplicable a toda idea compleja): instrumentar → métrica
offline contra ground truth → matriz de variantes → solo entonces Elo, una
variable por vez, con presupuesto de persistencia explícito (6-10 SPRTs por
familia, no 1).

## Fase 0 — instrumentación (sin coste Elo)

Contadores por búsqueda (detrás de knob debug): spells generados / clase early
/ clase late / dropped, por profundidad; y en el arnés offline: sobre las ~82k
posiciones PSV con cast en la PV (ground truth de "spell bueno"), medir para
cada variante del clasificador:
- **recall**: % de PV-casts clasificados early
- **class-size**: % del total de spells legales clasificados early
Un clasificador útil para staging: recall alto (>85%?) con class-size BAJO
(5-15%). El v1 probablemente ronda recall ~99% / class-size ~50-70% (a medir).

## Fase 1 — matriz de criterios (offline, sin partidas)

Endurecimientos AND, cada uno un bit de la matriz (se miden todas las
combinaciones sobre PSV, es un script, no cuesta nada):
- **C1 (ubdip literal)**: freeze-al-rey exige rey-rival-atacado (directo O por
  jump-checker — la rama jump-checkers ya computa esto).
- **C2**: freeze-silenciador exige que el atacante silenciado ataque de verdad
  (no solo "es atacante": SEE del cambio que amenaza > 0 o pieza mayor).
- **C3**: freeze-de-material exige que el material congelado esté atacable por
  nosotros el próximo ply (no solo "es mayor").
- **C4**: jump-reveal con umbral variable (400 / 800 / 1200) y exigiendo que el
  reveal apunte a pieza defendida-insuficiente o rey.
- **C5**: el logit del policy head como criterio continuo (umbral barrible) —
  la señal AUC 0.846 usada como FRONTERA, no como bonus.
Salida: frontera de Pareto recall/class-size; elegir 2-3 puntos.

## Fase 2 — matriz de colocación (SPRTs baratas, STC, una variable por vez)

Con el criterio fijado en un punto Pareto:
- **B1**: early tras good-captures (v1) vs tras killers vs interleaved por score.
- **B2**: late tras bad-quiets (v1) vs late con reducción extra (+1R) vs
  late saltable por LMP a depth<umbral.
- **B3**: dropped = solo useless (v1) vs dropped también los bottom-k% del
  policy logit.
Presupuesto: 6 SPRTs [1.00, 6.00] STC. Gate de familia: si NINGUNA variante
con class-size <20% y recall >85% da >0 tras 6 intentos, la familia muere con
evidencia (y quedará la nota de que se exploró el espacio, no un punto).

## Fase 3 — SPSA fino sobre la variante ganadora (si la hay) y LTC.

## Reparto de ejecución
- Codex (ayudante dev): arnés offline de recall/class-size sobre PSV, los
  parches C1-C5/B1-B3 como knobs, contadores de instrumentación.
- Yo: diseño, lectura de Pareto, decisiones de qué encolar, autopsias.
- Granja: solo ve las variantes que sobreviven el filtro offline.

## Nota sobre persistencia calibrada (ubdip 20:37)
"Ni intentar sin fin ni rendirse a la primera": el presupuesto explícito por
familia (arriba) es la implementación operativa de eso. Se declara ANTES de
empezar y se respeta en ambas direcciones.
