# S6 — Análisis de superficie: port XBoard/CECP a Spell-Stockfish

Fuentes analizadas:
- Referencia: `FSF-spell-baseline/src/xboard.{h,cpp}` (StateMachine CECP), `uci.cpp` (dispatch de protocolo, notación), `search.cpp` (salida `move`/thinking/resultados), `ucioption.cpp` (features de opciones y comando `setup`), `variant.cpp` (spell_chess_variant).
- Nuestro motor: `Spell-Stockfish/src/uci.cpp`, `src/uci.h`, `src/engine.h`, `src/main.cpp`.

---

## 1. Comandos CECP que usa realmente una GUI típica

Lo que la referencia implementa (y es el subconjunto que WinBoard/XBoard, cutechess-cli y polyglot ejercitan de verdad):

| Grupo | Comandos | Notas |
|---|---|---|
| Handshake | `xboard`, `protover 2` → línea `feature ...` + `feature done=1`; `accepted`/`rejected` (ignorar) | Obligatorio |
| Partida | `new`, `force`, `go`, `?` (move now), `result <res> {...}` | FSF trata `result` igual que `force` (para search + playColor=ninguno). No implementa `playother` (colors=0 lo hace innecesario) |
| Posición | `setboard <FEN>`, `usermove <mv>` (y move "pelado" si la GUI no aceptó usermove), `undo`, `remove` | `undo`=1 ply, `remove`=2 plies; ambos relanzan análisis si `UCI_AnalyseMode` |
| Análisis | `analyze`, `exit`, `.` (opcional, FSF no lo implementa) | analyze = búsqueda infinita que se reinicia con cada move/undo |
| Relojes | `level MPS BASE INC` (BASE puede ser `m:ss`), `st N` (s/jugada), `sd N` (depth), `time N` / `otim N` (**centisegundos**) | `time`/`otim` llegan antes del usermove; asignación por playColor, no por side_to_move |
| Config | `cores N`→Threads, `memory N`→Hash, `hard`/`easy`→Ponder, `option Name=Value`, `ping N`→`pong N` | |
| Variantes | `variant spell-chess` → set variante + reset posición; el motor responde con `setup ...` (variante no estándar) | |
| Extra FSF (no CECP) | `d`, `eval`, `perft N`; `lift`/`hover`/`put` + `highlight` (feature highlight=1, GUI interactiva); `partner`/`ptell`/`holding` (solo bughouse) | Highlight y bughouse **fuera de alcance v1** |

Salidas del motor hacia la GUI:
- `move <mv>` tras búsqueda de juego (no en analyze/ponder) — `search.cpp:345`.
- Thinking output (post): `depth score time(cs) nodes seldepth nps tbhits \t pv...` — `search.cpp:2100`. Score en **centipawns enteros planos**; mate = `(XBOARD_VALUE_MATE=200000 + distancia)` (`uci.cpp:436`, `types.h:356`).
- Reclamo de resultado: `1-0 {White wins}` / `0-1 {Black wins}` / `1/2-1/2 {Draw}` cuando no hay jugadas legales o fin opcional de partida (`search.cpp:256-271`).
- `pong N`, `Hint: <mv>` (al entrar en ponder), `Illegal move: <mv>`, `Error (unknown command): <tok>`.

## 2. Cómo maneja FSF `variant spell-chess` y la notación en CECP

- La variante está registrada como **`spell-chess`** (`variant.cpp:1878`) y **no** está en `standard_variants` (`ucioption.cpp:48`), así que al recibir `variant spell-chess` el on_change envía el comando **`setup (<pieceToCharTable>) 8x8+<pocket>_spell-chess <startFen>`** para que la GUI aprenda tablero, pockets y FEN inicial (`ucioption.cpp:96-115`). El string exacto conviene capturarlo del oráculo (`FSF_Spell_test_baseline.exe` con `xboard`+`protover 2`+`variant spell-chess`).
- **Notación de moves: idéntica a UCI, coordenadas, NO SAN.** En `UCI::move()` (`FSF uci.cpp:510-578`) el prefijo de poción `<char>@<sq>,` se construye **sin condicional de protocolo** (a diferencia de walling, que sí bifurca por XBOARD). Es decir, en CECP el gated move viaja exactamente como `usermove f@e4,e2e4` / `usermove j@d6,d7d5`, y el motor responde `move f@e4,e2e4`. FSF no negocia `san=1`, por lo que WinBoard usa coordenadas por defecto.
- `to_move()` parsea por fuerza bruta comparando contra `UCI::move()` de cada legal — **nuestro `UCIEngine::to_move` ya hace exactamente lo mismo** y nuestro `UCIEngine::move` ya emite `f@.../j@...` (`Spell-Stockfish/src/uci.cpp:776-800`). Conversión de notación: **coste cero**.
- `@@@@` (pass) en XBOARD no aplica: spell-chess no tiene pass. La lógica especial de `setboard`-como-pass de FSF (`xboard.cpp:304-334`) se puede omitir.

## 3. Features que negocia FSF (protover 2)

```
feature setboard=1 usermove=1 time=1 memory=1 smp=1 colors=0 draw=0
        highlight=1 name=0 sigint=0 ping=1 myname="..." variants="chess,...,spell-chess,..."
feature option="<Name> -<type> <default> [min max | /// combos]"   (una por opción, excluye UCI_Variant/Threads/Hash)
feature done=1
```
Para nosotros (motor mono-variante): igual pero `variants="spell-chess"`, `highlight=0` (o simplemente no emitirla) en v1, y `myname="Spell-Stockfish <ver>"`. `draw=0` (nunca ofrecer/aceptar tablas), `sigint=0`, `colors=0` (evita white/black legacy), `name=0`.

## 4. Mapa de integración con nuestro `Engine` (engine.h)

Diferencia estructural clave: FSF mete el estado XBoard dentro del hilo de búsqueda (`search.cpp` imprime `move`, hace `stateMachine->do_move`, etc.). Nuestro SF moderno tiene un `Engine` encapsulado con callbacks — el port debe ser un **adaptador externo** que NO toque search.cpp.

APIs de `Engine` que ya bastan:
| Necesidad CECP | API existente |
|---|---|
| Arrancar búsqueda (go/analyze/st/sd/level) | `engine.go(LimitsType&)` — LimitsType ya tiene `time[]`, `inc[]`, `movestogo`, `depth`, `movetime`, `infinite` |
| Parar (`force`, `result`, `?`, `exit`, nuevo comando) | `engine.stop()` + `engine.wait_for_search_finished()` |
| `new` | `engine.search_clear()` + `set_position(StartFEN, {})` |
| `setboard` / `usermove` / `undo` / `remove` | `engine.set_position(fen, moves)` — el adaptador guarda `startFen` + `vector<string> moveList` y re-monta (undo = pop_back + re-set). Devuelve `optional<PositionSetError>` → manejable sin matar el proceso |
| `cores`/`memory`/`hard`/`easy`/`option` | `engine.get_options()["Threads"/"Hash"/"Ponder"/...]` |
| Interceptar bestmove → `move ...` | `engine.set_on_bestmove(cb)` |
| Thinking output XBoard | `engine.set_on_update_full(cb)` reformateando `InfoFull` (depth, score→cp plano/200000+mate, timeMs/10, nodes, selDepth, nps, tbHits, pv) |
| Silenciar ruido UCI (`info string ...` rompe GUIs CECP) | `set_on_iter`, `set_on_update_no_moves`, `set_on_verify_network`, y el `add_info_listener` de opciones — patrón ya probado en `datagen()` (`uci.cpp:376-391`) |
| Ponder (hard) | `limits.ponderMode` + `engine.set_ponderhit(bool)` — fase 2 |

Lo que falta (todo implementable en el adaptador, sin tocar Engine):
1. **Espejo de posición local**: el adaptador necesita su propia `Position` + `StateListPtr` para (a) parsear `usermove` con `UCIEngine::to_move(pos, str)`, (b) saber side_to_move para `time`/`otim`, (c) detectar fin de partida. Patrón idéntico ya existe en `datagen()`.
2. **Reclamo de resultados**: nuestro Engine con 0 legales emite `bestmove (none)`; el adaptador debe pre-chequear `MoveList<LEGAL>` en el espejo antes de `go` y emitir `1-0/0-1/1/2-1/2 {...}` (en spell: sin rey = derrota por extinción; sin moves legales bajo jaque/ahogado según regla spell).
3. **Flag de aborto**: `force`/`result`/`new` durante búsqueda deben tragarse el `move` — el callback on_bestmove dispara igualmente tras `stop()`; hace falta `bool discardBestmove` en el adaptador (equivalente al `Threads.abort` de FSF).
4. **Modo analyze**: flag propio (no hay `UCI_AnalyseMode` en SF moderno... verificar; si no existe, un bool del adaptador basta) + relanzar `go(infinite)` tras cada move/undo.

## 5. Estimación de piezas

| Pieza | Tamaño estimado | Notas |
|---|---|---|
| `src/xboard.h` — clase `XBoardEngine`/adaptador (Engine&, Position espejo, moveList, limits, playColor, flags analyze/discard) | ~70 líneas | Espejo del StateMachine de FSF sin bughouse |
| `src/xboard.cpp` — `process_command()` completo | ~300–350 líneas | Sin partner/holding/highlight/pass: el switch de FSF son 320 líneas con todo eso incluido |
| Parser de relojes CECP → `LimitsType` | ~40 líneas (dentro del anterior) | `level` (con `m:ss`), `st`, `sd`, `time`/`otim` ×10 ms, asignación por playColor — copiar lógica de FSF `xboard.cpp:259-303` |
| Conversión de notación | **0 líneas** | `UCIEngine::move/to_move` ya emiten/parsean `f@e4,e2e4` |
| Formateador thinking output + score XBoard (cp plano, mate 200000±) | ~35 líneas | Callback on_update_full alternativo |
| Detección de fin de partida + strings resultado | ~30 líneas | Sobre el espejo |
| Dispatch en `uci.cpp::loop()` | ~10 líneas | token `xboard` → modo CECP; mientras activo, delegar cada línea al adaptador (mantener `quit`) |
| Línea `feature ...` + `setup ...` de spell-chess | ~25 líneas | Capturar setup exacto del oráculo |
| Ponder CECP (`hard`) — fase 2 | ~60 líneas | Hint:, ponder-hit/miss con undo |
| Tests: sección XBoard de `protocol.sh` (S5) vs transcript dorado del oráculo | ~80 líneas script | handshake, new/level/usermove-gated/go/undo/setboard/analyze/ping/result |

**Total v1 (sin ponder ni highlight): ~500–550 líneas nuevas + script de test. Esfuerzo: 1–2 sesiones.**

### Estructura de archivos propuesta
```
src/xboard.h        — class XBoardEngine { Engine& engine; Position pos; StateListPtr states;
                       std::vector<std::string> moveList; std::string curFen;
                       Search::LimitsType limits; Color playColor; bool analyzeMode, discardBestmove;
                       void process_command(const std::string&, std::istringstream&); ... }
src/xboard.cpp      — implementación (features, relojes, estado force/go/analyze, resultados)
src/uci.cpp         — en loop(): if (token=="xboard") { xboardMode=true; instalar callbacks CECP; }
                       else if (xboardMode) xb->process_command(token, is);   (quit sigue global)
src/uci.h           — miembro std::unique_ptr<XBoardEngine> + flag
tests/protocol.sh   — sección xboard con diff contra transcript del oráculo
```

## 6. Riesgos

(ver lista estructurada; los tres mayores: ponder CECP, desync de estado tras stop, y `terminate_on_critical_error` fatal en setboard inválido)

## 7. Próximas acciones

(ver lista estructurada)

## Riesgos (lista estructurada)

- Ponder CECP ('hard'): FSF juega el ponder move en el tablero interno y lo deshace en ponder-miss (xboard.cpp:58-83); mapear eso al ponderMode/set_ponderhit de nuestro Engine es la mayor fuente de desyncs posición-GUI. Mitigación: v1 sin ponder (ignorar 'hard'), fase 2 separada con tests propios.
- Desync tras stop: on_bestmove dispara siempre al parar la búsqueda; si 'force'/'result'/'new' no activan el flag de descarte antes de stop(), se emite un 'move' espurio que corrompe la partida en la GUI (equivalente al Threads.abort de FSF).
- terminate_on_critical_error hace exit(1) ante FEN inválido; un 'setboard' malformado de la GUI mataría el motor. CECP espera 'tellusererror Illegal position' y seguir vivo — usar el retorno optional<PositionSetError> de set_position, nunca la ruta fatal.
- Rebuild por FEN pierde historial de repeticiones: el adaptador debe re-montar siempre desde startFen + moveList completa (no desde un snapshot engine.fen()), o los reclamos de tablas por triple repetición serán incorrectos.
- Ruido UCI en modo CECP: 'info string ...' (NUMA, red NNUE, datagen) confunde a WinBoard/cutechess; hay que silenciar o redirigir todos los listeners al entrar en modo xboard.
- String exacto de 'setup' para spell-chess: si difiere del oráculo la GUI montará mal pockets/tablero; capturarlo empíricamente del baseline antes de codificarlo.
- time/otim llegan antes del usermove y en centisegundos; asignarlos por side_to_move en vez de por playColor invierte los relojes en cuanto hay force/undo (FSF lo resuelve con playColor, copiar tal cual).
- Convención de mate XBoard (200000+d, cp planos sin 'cp '): si se emite formato UCI en el thinking output, las GUIs adjudican mal.
- Reclamo de resultados en spell: las reglas de fin (extinción del commoner, stall bajo ataque) difieren del ajedrez estándar; el pre-chequeo del adaptador debe usar exactamente la semántica de nuestra Position (patrón ya validado en datagen uci.cpp:405-421).


## Próximas acciones

- Capturar transcript dorado del oráculo: pipe 'xboard / protover 2 / variant spell-chess / new / level 40 5 0 / usermove e2e4 / ...' a FSF_Spell_test_baseline.exe y guardar features+setup+formato de move/thinking/resultados como fixture de protocol.sh.
- Implementar src/xboard.{h,cpp} v1 (sin ponder ni highlight) + dispatch en uci.cpp::loop con instalación de callbacks CECP y silenciado de info strings.
- Añadir sección XBoard a protocol.sh (gate S5): handshake, partida con moves gated f@/j@, undo/remove, setboard, analyze/exit, ping, relojes level/st/sd/time/otim, reclamo de resultado; diff contra el transcript del oráculo.
- Smoke test manual en WinBoard con variant spell-chess (verificar que el comando setup monta pockets F/J) y en cutechess-cli -protocol xboard motor-vs-oráculo, >100 partidas por el protocolo de 3 TCs.
- Fase 2: ponder CECP ('hard', Hint:, ponder-hit/miss) mapeado a limits.ponderMode + set_ponderhit; opcional: highlight=1 (lift/hover/put) si se quiere paridad total con la referencia.
