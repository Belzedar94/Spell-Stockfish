"""Big bet 2: train the tiny cast-gate policy head (owner green-lit).

Dataset: PSV records whose PV move casts (tools/policy_extract.py stats).
Sample = (position, gate) pairs: the played gate as positive, K random other
gates as negatives. Features are DELIBERATELY the ones the engine can compute
in two bitboard ops per gate (they must match src inference exactly):

  0 isFreeze          1 cheb(gate, enemyK)/7   2 cheb(gate, ourK)/7
  3 enemyMat_zone/16  4 ownMat_zone/16         5 enemyCnt_zone/4
  6 enemyKingInZone   7 phase=min(ply,100)/100 8 handF_us/5
  9 handJ_us/2       10 centerDist(gate)/3.5

Model: 11 -> 24 -> 1 MLP (tanh), BCE. Exports src/spellpolicy/policy_weights.h.

Usage: python tools/policy_train.py <run.bin> [--sample 4] [--negatives 3]
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psv_decode import decode  # noqa: E402

VALS = [1.0, 3.0, 3.0, 5.0, 9.0, 0.0]  # P N B R Q K


def zone_squares(gate, freeze):
    if not freeze:
        return [gate]
    f, r = gate & 7, gate >> 3
    return [rr * 8 + ff for rr in range(max(0, r - 1), min(7, r + 1) + 1)
            for ff in range(max(0, f - 1), min(7, f + 1) + 1)]


def features(d, gate, spell_bits):
    stm = d["stm"]
    them = 1 - stm
    ek = d["bk"] if stm == 0 else d["wk"]
    ok = d["wk"] if stm == 0 else d["bk"]
    freeze = spell_bits == 1

    emat = omat = ecnt = 0.0
    ekin = 0.0
    for sq in zone_squares(gate, freeze):
        if sq == ek:
            ekin = 1.0
        pc = d["board"].get(sq)
        if pc is None:
            continue
        v = VALS[pc[0]] if pc[0] < 6 else 0.0
        if pc[1] == them:
            emat += v
            ecnt += 1
        elif pc[1] == stm:
            omat += v

    def cheb(a, b):
        return max(abs((a & 7) - (b & 7)), abs((a >> 3) - (b >> 3)))

    gf, gr = gate & 7, gate >> 3
    center = max(abs(gf - 3.5), abs(gr - 3.5))
    hands = d["hands"][stm]
    return [1.0 if freeze else 0.0,
            cheb(gate, ek) / 7.0 if ek < 64 else 1.0,
            cheb(gate, ok) / 7.0 if ok < 64 else 1.0,
            min(emat, 16.0) / 16.0, min(omat, 16.0) / 16.0, min(ecnt, 4.0) / 4.0,
            ekin, min(d["ply"], 100) / 100.0,
            hands[6] / 5.0, hands[7] / 2.0, center / 3.5]


def main():
    src = sys.argv[1]
    sample = 4
    negs = 3
    if "--sample" in sys.argv:
        sample = int(sys.argv[sys.argv.index("--sample") + 1])
    if "--negatives" in sys.argv:
        negs = int(sys.argv[sys.argv.index("--negatives") + 1])

    rng = random.Random(7)
    X, Y = [], []
    n = kept = 0
    with open(src, "rb") as f:
        while True:
            rec = f.read(76)
            if len(rec) < 76:
                break
            n += 1
            if n % sample:
                continue
            move = struct.unpack_from("<I", rec, 68)[0]
            spell_bits = (move >> 16) & 3
            if not spell_bits:
                continue
            d = decode(rec)
            gate = (move >> 18) & 0x3F
            X.append(features(d, gate, spell_bits))
            Y.append(1.0)
            for _ in range(negs):
                g = rng.randrange(64)
                while g == gate:
                    g = rng.randrange(64)
                X.append(features(d, g, spell_bits))
                Y.append(0.0)
            kept += 1

    print(f"posiciones cast usadas: {kept:,} -> ejemplos {len(X):,}")

    import torch
    import torch.nn as nn
    torch.manual_seed(7)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    Xt = torch.tensor(X, dtype=torch.float32, device=dev)
    Yt = torch.tensor(Y, dtype=torch.float32, device=dev).unsqueeze(1)
    ntr = int(len(Xt) * 0.9)
    perm = torch.randperm(len(Xt), device=dev)
    tr, va = perm[:ntr], perm[ntr:]

    model = nn.Sequential(nn.Linear(11, 24), nn.Tanh(), nn.Linear(24, 1)).to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=3e-3)
    lossf = nn.BCEWithLogitsLoss()
    for epoch in range(6):
        model.train()
        for i in range(0, len(tr), 16384):
            idx = tr[i:i + 16384]
            opt.zero_grad()
            loss = lossf(model(Xt[idx]), Yt[idx])
            loss.backward()
            opt.step()
        model.eval()
        with torch.no_grad():
            out = model(Xt[va])
            # AUC via rank statistic
            o = out.squeeze(1)
            pos, neg = o[Yt[va].squeeze(1) > 0.5], o[Yt[va].squeeze(1) < 0.5]
            auc = (pos.unsqueeze(1) > neg.unsqueeze(0)).float().mean().item()
            print(f"epoch {epoch}: val AUC {auc:.4f}")

    # Export C header
    W1 = model[0].weight.detach().cpu().numpy()
    B1 = model[0].bias.detach().cpu().numpy()
    W2 = model[2].weight.detach().cpu().numpy()
    B2 = model[2].bias.detach().cpu().numpy()
    os.makedirs(os.path.join(os.path.dirname(__file__), "..", "src", "spellpolicy"),
                exist_ok=True)
    hp = os.path.join(os.path.dirname(__file__), "..", "src", "spellpolicy",
                      "policy_weights.h")
    with open(hp, "w", encoding="ascii") as h:
        h.write("// Generated by tools/policy_train.py - tiny cast-gate policy head\n")
        h.write("// (11 features -> 24 tanh -> 1 logit). Do not edit by hand.\n")
        h.write("#ifndef SPELL_POLICY_WEIGHTS_H\n#define SPELL_POLICY_WEIGHTS_H\n\n")
        h.write("namespace Stockfish::SpellPolicy {\n\n")
        h.write("constexpr int IN = 11, HID = 24;\n\n")
        def dump(name, arr):
            flat = arr.flatten()
            h.write(f"constexpr float {name}[{len(flat)}] = {{\n    ")
            h.write(", ".join(f"{v:.6f}f" for v in flat))
            h.write("};\n\n")
        dump("W1", W1)
        dump("B1", B1)
        dump("W2", W2)
        dump("B2", B2)
        h.write("}  // namespace Stockfish::SpellPolicy\n\n#endif\n")
    print("header:", hp)


if __name__ == "__main__":
    main()
