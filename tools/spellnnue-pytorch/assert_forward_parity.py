#!/usr/bin/env python3
"""El eslabon que faltaba en la cadena de gates (auditoria 17-ago): comparar
el forward de ENTRENAMIENTO (float con quantized_activations=True) contra el
forward EXPORTADO (entero, quantized_forward sobre quantized_params), que es
lo que parity.py ya compara con el motor a diff cero.

Con este assert en verde y parity.py en verde, la cadena entrenamiento ->
export -> motor queda cerrada de punta a punta. La diferencia esperada es
solo el redondeo de pesos al cuantizar (unos pocos cp); una diferencia
sistematica o un factor multiplicativo es un bug de escala como el que costo
la generacion 2.

Uso: assert_forward_parity.py --checkpoint X.pt --data corpus.run7 [--count 1000]
"""

import argparse
import mmap
import sys

import torch

import features
import model
import run7


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--count", type=int, default=1000)
    # Linea base MEDIDA sobre la red buena conocida (v2run1, 17-ago):
    # diff_media 11.18cp, diff_max 87cp, pendiente 1.0010. El gap es el ruido
    # de cuantizar a int8 con 60-80 features activas, no un defecto; lo que
    # este gate caza son REGRESIONES contra esa base y factores de escala.
    parser.add_argument("--max-cp", type=float, default=15.0,
                        help="techo de |diff| medio tolerado (base v2run1: 11.2)")
    args = parser.parse_args()

    net = model.SpellNNUE()
    payload = torch.load(args.checkpoint, map_location="cpu",
                         weights_only=False)
    net.load_state_dict(payload["model"] if "model" in payload else payload)
    net.eval()
    params = model.quantized_params(net)

    suma = peor = 0.0
    suma_ratio_num = suma_ratio_den = 0.0
    vistos = 0
    with open(args.data, "rb") as file:
        disponibles, _, _ = run7.read_header(file)
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
            paso = max(1, disponibles // args.count)
            for index in range(0, disponibles, paso):
                offset = run7.HEADER_SIZE + index * run7.RECORD_SIZE
                record = run7.unpack(mapped[offset: offset + run7.RECORD_SIZE])
                if run7.W_KING not in record.board or run7.B_KING not in record.board:
                    continue
                item = features.extract(record)
                batch = model.SparseBatch.from_features(
                    [item], [record.stm], [0.0], [0])
                with torch.no_grad():
                    flotante = float(net(batch, quantized_activations=True)[0])
                entero = model.quantized_forward(params, item, record.stm)[2]
                diff = abs(flotante - entero)
                suma += diff
                peor = max(peor, diff)
                suma_ratio_num += flotante * entero
                suma_ratio_den += entero * entero
                vistos += 1
                if vistos >= args.count:
                    break

    media = suma / max(1, vistos)
    pendiente = suma_ratio_num / max(1.0, suma_ratio_den)
    print(f"posiciones={vistos} diff_media={media:.2f}cp diff_max={peor:.2f}cp "
          f"pendiente_float~entero={pendiente:.4f}")
    if media > args.max_cp or not 0.97 <= pendiente <= 1.03:
        print("FALLO: divergencia sistematica entre el forward entrenado y el "
              "exportado", file=sys.stderr)
        raise SystemExit(1)
    print("OK: forward de entrenamiento y forward exportado coinciden salvo "
          "redondeo")


if __name__ == "__main__":
    main()
