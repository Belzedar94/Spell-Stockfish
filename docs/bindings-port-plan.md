# S6 — Análisis de superficie: bindings Python (pyffish) y JS/WASM (ffish.js)

Fuentes: `FSF-spell-baseline/src/pyffish.cpp` (482 líneas), `setup.py`, `.github/workflows/wheels.yml`, `src/ffishjs.cpp` (772 líneas), `src/Makefile_js`, `.github/workflows/ffishjs.yml`, `tests/js/*`; nuestro motor: `src/engine.h`, `src/uci.{h,cpp}`, `src/position.{h,cpp}`, `src/xboard.{h,cpp}`; `PLAN.md:33` (fila S6), formato según `docs/xboard-port-plan.md`.

## 1. Superficie pyffish (referencia) y qué puede servir nuestro motor

Diseño clave: pyffish es **stateless y solo-reglas** — cada llamada recibe `(variant, fen, moveList)` y remonta la posición desde cero (`buildPosition`, `pyffish.cpp:27-54`); nunca invoca búsqueda ni NNUE (`setup.py:15` compila con `-DNNUE_EMBEDDING_OFF`). Tabla de métodos completa en `pyffish.cpp:405-427`:

| Función pyffish | Internals FSF que usa | Servible con nuestra API hoy |
|---|---|---|
| `legal_moves` | `MoveList<LEGAL>` + `UCI::move` (:221-227) | ✅ idéntico: `MoveList<LEGAL>` + `UCIEngine::move` con prefijos `f@/j@` (`uci.cpp:816-819`) |
| `get_fen` | `pos.fen(sfen,showPromoted,countStarted)` (:247) | ✅ `Position::fen()` sin args (`position.h:125`); emite holdings + bloque `{F@e4:3,...}` (`position.cpp:720-739`) byte-idéntico al oráculo (gate S1) |
| `gives_check` | `Stockfish::checked(pos)` de apiutil (:262) | ✅ `pos.checkers()` (`position.h:389`) |
| `game_result` | `is_immediate_game_end`/`checkmate_value`/`stalemate_value` (:311-319) | ⚠️ semántica distinta: spell = captura-del-rey; mapeo ya resuelto en `xboard.cpp:95-122` (sin rey = derrota, stall en jaque = derrota, stall quieto = tablas) — reusar tal cual |
| `is_capture` | `pos.capture(to_move(...))` (:279) | ✅ `Position::capture` (`position.h:429`) |
| `validate_fen` | `FEN::validate_fen` → enum (:385) | 🔶 nuestro validador es `Position::set` → `optional<PositionSetError>` (`position.h:123`); devolver 1/0 + mensaje. OJO compat: pyffish invierte el orden de args a `(fen, variant)` (:381) |
| `start_fen`, `version`, `info`, `set_option` | Variant registry / Options (:111-119, :80-97) | ✅ constante `StartFEN` (`uci.h:40-41`), `engine_info()`; opciones: subconjunto útil |
| `get_san`, `get_san_moves` | `SAN::move_to_san` (apiutil.h, 1245 líneas) | ❌ no tenemos SAN; además apiutil.h **no contiene ninguna referencia a spell/gate** (grep 0 hits) → el SAN del baseline no es spell-aware; tratarlo como fase 2 opcional |
| `variants`, `load_variant_config`, `two_boards`, `captures_to_hand`, `piece_to_partner`, `is_optional_game_end`, `has_insufficient_material`, `get_fog_fen` | Framework multivariante FSF | 🔶 stubs mono-variante: `variants()=["spell-chess"]`, `two_boards/captures_to_hand=False`, `piece_to_partner=""`; `is_optional_game_end` ≈ `pos.is_draw/is_repetition`; `has_insufficient_material`: teoría distinta en captura-del-rey → `(False,False)` conservador v1 |

## 2. Superficie ffish.js (embind)

A diferencia de pyffish, ffish.js es **stateful**: clase `Board` con `Position + StateListPtr + moveStack` (`ffishjs.cpp:79-457`) — exactamente el patrón espejo que ya validamos en `XBoardEngine` (`xboard.h:75-83`). Métodos ligados en `ffishjs.cpp:687-772`: `legalMoves`, `push/pushMoves/pop/reset`, `fen/setFen`, `turn/fullmoveNumber/halfmoveClock/gamePly`, `isCheck/checkedPieces`, `isGameOver/result`, `isCapture`, `moveStack`, `pocket`, `toString`, más `Game`/`readGamePGN` (parser PGN sobre `push_san`, :563-683) y globales `variants/setOption/loadVariantConfig/validateFen`. Todo lo no-SAN es servible con nuestra API; `pushSan/legalMovesSan/variationSan/readGamePGN` dependen de SAN (fase 2).

## 3. Mapa de integración con nuestro motor

- **No se necesita `Engine`**: los bindings son capa-Position pura. Init requerido: `Bitboards::init() + Attacks::init() + Position::init()` (`main.cpp:43-45`), sin threads (nuestro `Position::set` ya no exige `Thread*`, a diferencia de FSF `pyffish.cpp:34`).
- **Moves gated gratis**: `UCIEngine::to_move` parsea por fuerza bruta contra `MoveList<LEGAL>` (`uci.cpp:832-840`), igual que FSF, así que `f@e4,e2e4` valida/parse sin código nuevo.
- **Prerequisito (paso 0)**: `position.cpp` incluye `uci.h` solo por `UCIEngine::square` (:103, :739, :763), y `uci.h` arrastra `engine.h` → search/NNUE/threads. Extraer `square/move/to_move` + `StartFEN` a un TU `src/notation.{h,cpp}` (~60 líneas movidas, firma bench intacta) desacopla el cierre de dependencias; es lo que permite el build WASM sin threads (ver §5).
- **Extensión propia recomendada**: accessors `spell_state()` (gate/cooldown/hand por color y hechizo, `position.h:323-339`) para que las GUIs no parseen el bloque `{}` del FEN.

## 4. Decisión clave: replicar API pyffish/ffish.js vs binding propio (pybind11/embind a medida)

| | Replicar superficie pyffish/ffish.js | API propia (pybind11 / embind nueva) |
|---|---|---|
| Ecosistema | ✅ pychess-variants consume pyffish y Fairyground consume ffish.js tal cual: drop-in con `import pyffish_spell as sf` | ❌ obliga a forkear cada consumidor |
| Dependencias | ✅ pyffish usa C-API pura de Python, cero deps (`pyffish.cpp:6`) | ❌ pybind11 requiere excepciones; nuestro build es `-fno-exceptions` (`Makefile:502`) → conflicto real |
| Costo | Copiar estructura probada (482+772 líneas de referencia) | Diseño desde cero + docs |
| Limpieza | ⚠️ hereda verrugas (args invertidos en `validate_fen`, stateless rebuild O(n) por llamada) | ✅ API moderna |

**Recomendación: replicar la superficie de referencia** (subconjunto sin SAN + stubs mono-variante + extensiones `spell_*`), C-API para Python y embind para JS. La razón de existir del hito es interoperar con las herramientas del ecosistema (PLAN.md, release 0.2); una API propia se puede añadir encima después sin romper nada.

## 5. Pipeline de build

- **Wheel Python**: clonar el patrón del baseline — `setup.py` con Extension compilando los SRCS (nuestra lista: `Makefile:79-85` menos `main.cpp`/`datagen.cpp`, + `pyffish.cpp`), flags `-std=c++17 -DNNUE_EMBEDDING_OFF` (+ `/std:c++17` MSVC, `setup.py:10-15`); CI con **cibuildwheel 2.22.0** matrix ubuntu/windows/macos + `CIBW_TEST_COMMAND=test.py` (`wheels.yml:13-31`) reutilizable casi literal. El wheel no incluye red: los bindings no evalúan.
- **WASM**: portar `Makefile_js` (emcc `--bind -DNNUE_EMBEDDING_OFF`, `TOTAL_MEMORY=32MB ALLOW_MEMORY_GROWTH` :26-49, variante `es6=yes` :58-60; salida `tests/js/ffish.js` :17; npm `ffish` 0.7.8 + `ffish-es6`, `tests/js/package.json:2-3`). Diferencia crítica: FSF compila con `-DNO_THREADS`, macro que **no existe** en SF moderno; hay que linkar solo el cierre reglas (attacks, bitboard, position, movegen, misc, spell*, notation, binding) y **no** thread/tt/search/engine — de lo contrario el wasm exige `-pthread` → SharedArrayBuffer/COOP/COEP en todo consumidor (nuestro `arch=wasm32` del Makefile:896-902 es para el motor completo, no para esto). Emsdk moderno (≥3.x), no el 1.39.16 anclado del baseline (`ffishjs.yml:8`).
- **La red de 86MB no es problema en S6**: los bindings de referencia jamás cargan red. El caso "motor jugable en browser" (red estándar 90.586.462 bytes `src/nn-0ee0657fb25e.nnue` + spell 101.788.576 bytes `spell-chess_run5rl_e10_l07.nnue`) es un hito aparte: incbin queda descartado por tamaño; opciones futuras: fetch → MEMFS + `setoption EvalFile` (ruta de carga ya generalizada en `engine.cpp:151-190`) o red distilled pequeña para web. Recomendación: excluirlo del alcance S6.

## 6. Estimación y orden de trabajo

| Paso | Pieza | Líneas est. |
|---|---|---|
| 0 | Extraer `src/notation.{h,cpp}` (square/move/to_move/StartFEN) + verificar cierre de deps con un main mínimo; gate: bench 2395529 intacto | ~80 (movidas) |
| 1 | Compilar el **pyffish del baseline** como oráculo local y congelar fixtures (fen/legal_moves/game_result sobre las 61 posiciones perft + partidas gated) | script ~60 |
| 2 | `src/pyffish.cpp` nuestro (C-API, superficie §1 sin SAN, + `spell_state`) + `setup.py` + `test.py` de paridad vs oráculo | ~380 + 50 + 150 |
| 3 | CI wheels (copiar `wheels.yml`) + publicación `pyffish-spell` | ~50 yml |
| 4 | `src/ffishjs.cpp` nuestro (embind, Board stateful sin SAN/PGN) + `Makefile_js` (subset sin threads) + tests mocha CJS/ESM de paridad | ~450 + 90 + 200 |
| 5 | Smoke tests de ecosistema: Fairyground con nuestro ffish.js, pychess con el wheel; fase 2 opcional: SAN spell-aware (~200) + PGN | — |

Total v1 (pasos 0-4): **~1.400-1.600 líneas**, 2-3 sesiones.

## 7. Riesgos

- **Cierre de dependencias del subset WASM**: `position.cpp` incluye `tt.h`/`syzygy/tbprobe.h`/`history.h`; si quedan símbolos duros tras el paso 0, el fallback es linkar el motor entero con `-pthread` (funciona, pero degrada la adoptabilidad del paquete npm). Verificarlo ANTES de escribir el binding (por eso es el paso 0).
- **StateInfo engordado**: `SpellAccumulator` mete ~4KB por estado (`position.h:88-94`); partidas de 500 plies ≈ 2MB de deque en WASM — dentro de `ALLOW_MEMORY_GROWTH`, pero medir.
- **Divergencia semántica silenciosa en los stubs** (`game_result`, `has_insufficient_material`, `is_optional_game_end`): pychess adjudica partidas con ellos; documentar cada desviación vs pyffish upstream y cubrirla en fixtures.
- **SAN**: si algún consumidor lo exige (pychess usa `get_san` para display), ni siquiera el baseline lo hace spell-aware — decidir formato (`f@e4,` + SAN base) con el oráculo humano (el propietario) antes de implementarlo.
- **`-fno-exceptions`**: prohíbe pybind11 y exige que el binding C-API nunca deje escapar throws (nuestro `Position::set` ya devuelve `optional`, `position.h:123`); en emscripten, `--bind` con excepciones deshabilitadas es el modo por defecto del baseline — mantenerlo.
- **Riesgo de empaquetado**: nombre PyPI/npm (`pyffish-spell`, `ffish-spell`) para no colisionar con upstream y poder coexistir instalados.
