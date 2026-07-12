#!/usr/bin/env python3
"""Hostile CECP sequences (S5 suite): regression tests for the adversarial
findings on the XBoard adapter. Every case reproduces a real bug that was
found by attacking the first build:

  1. 'quit' during a game search  -> was a use-after-free (0xC0000005)
  2. 'option'/'easy' during analyze -> was a permanent deadlock
  3. malformed 'level'            -> was std::terminate via std::stoi
  4. '.' during analyze           -> killed the analysis permanently
  5. junk token during analyze    -> killed the analysis permanently
  6. ping while thinking          -> pong must arrive AFTER the move

Usage: python xboard_hostile_test.py [path-to-engine]
"""

import subprocess
import sys
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "../src/stockfish.exe"

FAILS = []


def check(cond, label):
    print(("ok  " if cond else "FAIL") + " " + label)
    if not cond:
        FAILS.append(label)


class E:
    def __init__(self):
        self.p = subprocess.Popen([ENGINE], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True,
                                  bufsize=1)

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def until(self, pred, tmo=60):
        out, dl = [], time.time() + tmo
        while time.time() < dl:
            line = self.p.stdout.readline()
            if not line:
                return out, False
            out.append(line.rstrip("\n"))
            if pred(out[-1]):
                return out, True
        return out, False

    def close(self, tmo=15):
        try:
            self.p.wait(timeout=tmo)
            return True
        except subprocess.TimeoutExpired:
            self.p.kill()
            return False


def boot(e):
    e.send("xboard")
    e.send("protover 2")
    _, ok = e.until(lambda l: l.startswith("feature done=1"), 120)
    return ok


def main():
    # --- 1. quit during a game search: clean exit, no access violation
    e = E()
    check(boot(e), "handshake (caso quit-durante-go)")
    e.send("new")
    e.send("go")
    time.sleep(0.3)  # search is running
    e.send("quit")
    clean = e.close()
    check(clean, "quit durante go: el proceso sale")
    if clean:
        check(e.p.returncode == 0,
              f"quit durante go: exit code 0 (fue {e.p.returncode})")

    # --- 2. option / easy during analyze: no deadlock, analysis resumes
    e = E()
    check(boot(e), "handshake (caso option-durante-analyze)")
    e.send("post")
    e.send("new")
    e.send("force")
    e.send("analyze")
    _, ok = e.until(lambda l: l[:1].isdigit(), 60)
    check(ok, "analyze emite thinking")
    e.send("option MultiPV=2")
    e.send("ping 7")
    out, ok = e.until(lambda l: l.startswith("pong 7"), 15)
    check(ok, "option durante analyze: no hay deadlock (pong 7)")
    _, ok = e.until(lambda l: l[:1].isdigit(), 15)
    check(ok, "el analisis se relanza tras option")
    e.send("easy")
    e.send("ping 8")
    _, ok = e.until(lambda l: l.startswith("pong 8"), 15)
    check(ok, "easy durante analyze: no hay deadlock (pong 8)")

    # --- 4./5. '.' and junk during analyze: analysis must survive
    e.send(".")
    e.send("garbagetoken")
    _, err = e.until(lambda l: l.startswith("Error (unknown command)"), 10)
    check(err, "token basura responde Error (unknown command)")
    _, ok = e.until(lambda l: l[:1].isdigit(), 15)
    check(ok, "el analisis sigue vivo tras '.' y basura")
    e.send("exit")
    e.send("quit")
    check(e.close(), "quit sale limpio tras analyze")

    # --- 3. malformed level: engine must not die
    e = E()
    check(boot(e), "handshake (caso level malformado)")
    e.send("level 0 x 0")
    e.send("level 40")
    e.send("ping 9")
    _, ok = e.until(lambda l: l.startswith("pong 9"), 10)
    check(ok, "level malformado no mata el proceso")
    e.send("quit")
    check(e.close(), "quit sale limpio tras level malformado")

    # --- 6. ping while thinking on our move: pong only AFTER the move
    e = E()
    check(boot(e), "handshake (caso ping-durante-go)")
    e.send("new")
    e.send("sd 12")
    e.send("st 3")
    e.send("go")
    time.sleep(0.2)
    e.send("ping 5")
    out, ok = e.until(lambda l: l.startswith("pong 5"), 30)
    check(ok, "pong 5 llega")
    move_idx = next((i for i, l in enumerate(out) if l.startswith("move ")), None)
    pong_idx = next((i for i, l in enumerate(out) if l.startswith("pong 5")), None)
    check(move_idx is not None and pong_idx is not None and move_idx < pong_idx,
          "el pong se difiere hasta despues del move")
    e.send("quit")
    check(e.close(), "quit final limpio")

    print(f"\n{'PASS' if not FAILS else 'FAIL'}: {len(FAILS)} fallos")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
