#!/bin/bash
# Spell Chess tactical regression runner.
#
# Plays every position of an EPD suite at a fixed depth and reports how many
# "bm" (best move) expectations the engine meets, plus every "am" (avoid move)
# it walks into.
#
# Usage:
#   ./run_tactics.sh <engine> [depth] [epd-file]
#
#   engine     path to any Spell-Stockfish binary (required)
#   depth      search depth, default 14
#   epd-file   suite to run, default spell_tactics.epd next to this script
#
# Environment:
#   SPELL_NET   path to the .nnue to load; when unset the script searches the
#               repository and the directories above it for a spell net, and
#               warns if it finds none (the engine would then silently score
#               these positions with the stock Stockfish network).
#   THREADS     search threads, default 1 (keep it at 1 for reproducible runs;
#               extra threads make the engine noticeably better at this suite,
#               so a score is only comparable against the same thread count)
#   HASH        hash in MB, default 256
#   STRICT      when set to 1, exit 1 if any position fails
#
# Exit status: 0 on a completed run (see STRICT), 2 on an operational error.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ENGINE="${1:-}"
DEPTH="${2:-14}"
EPD="${3:-${SCRIPT_DIR}/spell_tactics.epd}"

THREADS="${THREADS:-1}"
HASH="${HASH:-256}"
STRICT="${STRICT:-0}"

die() { echo "run_tactics: $*" >&2; exit 2; }

[ -n "$ENGINE" ] || die "usage: $0 <engine> [depth] [epd-file]"
[ -x "$ENGINE" ] || command -v "$ENGINE" >/dev/null 2>&1 || die "engine not executable: $ENGINE"
[ -f "$EPD" ] || die "suite not found: $EPD"
case "$DEPTH" in
  ''|*[!0-9]*) die "depth must be a positive integer, got: $DEPTH" ;;
esac

# Locating the net matters: with no EvalFile the engine falls back to the stock
# Stockfish network, which evaluates Spell Chess positions as nonsense while
# still happily returning a bestmove. Look next to the repository, next to the
# binary, and up the tree (git worktrees live several levels below the clone
# that holds the net).
NET="${SPELL_NET:-}"
if [ -z "$NET" ]; then
  engine_dir="$(cd "$(dirname "$ENGINE")" 2>/dev/null && pwd)" || engine_dir=""
  dir="$REPO_ROOT"
  for _ in 1 2 3 4 5 6 7 8; do
    for cand in "$dir"/spell-chess_*.nnue "$dir"/spell*.nnue; do
      [ -f "$cand" ] && { NET="$cand"; break 2; }
    done
    [ "$dir" = "/" ] && break
    dir="$(dirname "$dir")"
  done
  if [ -z "$NET" ] && [ -n "$engine_dir" ]; then
    for cand in "$engine_dir"/spell*.nnue "$engine_dir"/../spell*.nnue; do
      [ -f "$cand" ] && { NET="$cand"; break; }
    done
  fi
fi

# A native Windows build cannot open the /c/... form this shell hands out, and
# it terminates instead of falling back, so translate before handing it over.
if [ -n "$NET" ] && command -v cygpath >/dev/null 2>&1; then
  NET="$(cygpath -m "$NET")"
fi

if [ -z "$NET" ]; then
  echo "run_tactics: WARNING no spell net found; the engine will fall back to" >&2
  echo "             its stock network and score these positions as nonsense." >&2
  echo "             Set SPELL_NET=<path to .nnue> to fix this." >&2
fi

# ---------------------------------------------------------------- read suite
# An EPD record is "<spell-FEN> bm <moves>; id "..."; c0 "...";". The spell FEN
# carries holdings and a {...} spell-state block, so the record is split at the
# first " bm " / " am " opcode rather than by counting FEN fields.
ids=(); fens=(); bms=(); ams=(); comments=()

while IFS= read -r line || [ -n "$line" ]; do
  line="${line%$'\r'}"   # survive a CRLF checkout on Windows
  case "$line" in
    ''|'#'*) continue ;;
  esac

  head=""
  case "$line" in
    *' bm '*) head="${line%% bm *}" ;;
  esac
  case "$line" in
    *' am '*)
      a="${line%% am *}"
      if [ -z "$head" ] || [ ${#a} -lt ${#head} ]; then head="$a"; fi
      ;;
  esac
  [ -n "$head" ] || die "record has neither bm nor am: $line"

  fen="$head"
  ops="${line:${#head}+1}"

  bm=""; am=""; id=""; comment=""
  # split the opcode section on ';'
  rest="$ops"
  while [ -n "$rest" ]; do
    op="${rest%%;*}"
    if [ "$op" = "$rest" ]; then rest=""; else rest="${rest#*;}"; fi
    # trim
    op="${op#"${op%%[![:space:]]*}"}"
    op="${op%"${op##*[![:space:]]}"}"
    [ -n "$op" ] || continue
    case "$op" in
      'bm '*) bm="${op#bm }" ;;
      'am '*) am="${op#am }" ;;
      'id '*) id="${op#id }"; id="${id%\"}"; id="${id#\"}" ;;
      'c0 '*) comment="${op#c0 }"; comment="${comment%\"}"; comment="${comment#\"}" ;;
    esac
  done

  ids+=("${id:-$((${#ids[@]} + 1))}")
  fens+=("$fen")
  bms+=("$bm")
  ams+=("$am")
  comments+=("$comment")
done < "$EPD"

n=${#fens[@]}
[ "$n" -gt 0 ] || die "no records in $EPD"

# ------------------------------------------------------------- run the suite
# One engine session. "go" returns to the input loop immediately, so what keeps
# the searches ordered and independent is the "ucinewgame" in front of each
# position: it waits for the previous search to finish and then clears the hash.
# The trailing one matters just as much, because "quit" does not wait and would
# otherwise cut the last search short and report whatever it had at the time.
feed() {
  echo "uci"
  echo "setoption name UCI_Variant value spell-chess"
  [ -n "$NET" ] && echo "setoption name EvalFile value $NET"
  echo "setoption name Threads value $THREADS"
  echo "setoption name Hash value $HASH"
  echo "isready"
  for ((i = 0; i < n; i++)); do
    echo "ucinewgame"
    echo "position fen ${fens[$i]}"
    echo "go depth $DEPTH"
  done
  echo "ucinewgame"
  echo "quit"
}

echo "Spell Chess tactical suite"
echo "  engine : $ENGINE"
echo "  net    : ${NET:-none found}"
echo "  suite  : $EPD ($n positions)"
echo "  depth  : $DEPTH   threads: $THREADS   hash: ${HASH}MB"
echo

trace="$(mktemp)"
trap 'rm -f "$trace"' EXIT
feed | "$ENGINE" | grep -E '^bestmove|Spell NNUE loaded|NNUE evaluation using|ERROR' > "$trace"

# The engine announces the stock network at every search start whether or not a
# spell net is active, so only the "Spell NNUE loaded" line proves the right one
# is in use. Without it the scores below are meaningless.
netused="$(grep -m1 'Spell NNUE loaded' "$trace" | sed 's/.*Spell NNUE loaded: //')"
if [ -n "$netused" ]; then
  echo "  loaded : $netused"
else
  echo "  loaded : NO SPELL NET, results are meaningless (set SPELL_NET)"
fi
echo

mapfile -t got < <(grep '^bestmove' "$trace" | awk '{print $2}')

if [ "${#got[@]}" -ne "$n" ]; then
  echo "run_tactics: engine returned ${#got[@]} of $n bestmoves" >&2
  grep 'ERROR' "$trace" | head -5 >&2
  echo "run_tactics: check the engine, the net and that the FENs parse" >&2
  exit 2
fi

# ---------------------------------------------------------------- score them
# A bm/am token matches the engine move literally, or as a glob so that a gated
# move can be accepted with any gate square, e.g. "f@*,d8d7" = Qd7 with any
# freeze gate.
matches() {
  local move="$1"
  local -a toks
  IFS=' ' read -r -a toks <<< "$2"
  local tok
  for tok in "${toks[@]}"; do
    # shellcheck disable=SC2053  # unquoted RHS is deliberate: glob match
    [[ "$move" == $tok ]] && return 0
  done
  return 1
}

pass=0
failed=()

for ((i = 0; i < n; i++)); do
  best="${got[$i]}"
  ok=1
  why=""
  if [ -n "${bms[$i]}" ] && ! matches "$best" "${bms[$i]}"; then
    ok=0; why="expected bm [${bms[$i]}]"
  fi
  if [ "$ok" = 1 ] && [ -n "${ams[$i]}" ] && matches "$best" "${ams[$i]}"; then
    ok=0; why="played avoid-move [${ams[$i]}]"
  fi
  if [ "$ok" = 1 ]; then
    pass=$((pass + 1))
    printf 'PASS  %-8s %s\n' "${ids[$i]}" "$best"
  else
    failed+=("${ids[$i]}")
    printf 'FAIL  %-8s got %-16s %s\n' "${ids[$i]}" "$best" "$why"
    [ -n "${comments[$i]}" ] && printf '                %s\n' "${comments[$i]}"
  fi
done

echo
echo "score: $pass/$n at depth $DEPTH"
if [ ${#failed[@]} -gt 0 ]; then
  echo "failed: ${failed[*]}"
fi

if [ "$STRICT" = "1" ] && [ "$pass" -ne "$n" ]; then
  exit 1
fi
exit 0
