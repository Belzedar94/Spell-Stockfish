> **Estado (2026-07-12), tras el análisis:**
> 1. El punto pendiente de la sección 4 ("extender el mapeo de variantes en el
>    cliente") **ya está implementado**: rama `spell-runner` del clon local del
>    fork (commit 2b8720c) añade la tabla `VARIANTS` en `Client/worker.py` y
>    `Client/uci_pair_runner.py` (runner UCI puro con salida compatible
>    cutechess, smoke-tested contra una réplica del parser del worker).
> 2. **Bloqueo real para engines privados**: el flujo `private: true` depende de
>    artifacts de GitHub Actions (`openbench.yml`), y Actions está bloqueado por
>    billing en `Belzedar94/Spell-Stockfish`. Hasta desbloquearlo: opciones =
>    (a) desbloquear billing, (b) hacer público el repo y usar la vía
>    `download_public_engine` (requiere shim `make` por defecto), o (c) parche
>    local del worker para usar binarios precompilados a mano.
> 3. Firmas bench medidas en el 5950X (net run5rl): suite S5
>    `bench 16 1 10 default depth` = 2395529 (con red) / 2785455 (sin red);
>    protocolo OpenBench `bench` a pelo (default depth) = 13456297 (sin red,
>    36 s — vigilar el timeout de 60 s del cliente en máquinas lentas).

# Runbook: despliegue LOCAL de OpenBench (fork openbench-spell) en Windows 10 + Python 3.12

Repo local: `C:\Users\djime\Documents\Chess_variants\Codex\Fairy-Stockfish organization\openbench-spell`
Origen del fork: `https://github.com/sscg13/OpenBench` (rama `shatranj` checked-out; es el fork de variantes que ya añadió shatranj a OpenBench de AndyGrant).

---

## 1. Dependencias y compatibilidad con Python 3.12 (Windows)

`requirements.txt` del servidor pide:

| Pin | Estado en Py 3.12 / Win | Acción |
|---|---|---|
| `Django==4.2.1` | Problemático: Django 4.2 solo soporta 3.12 oficialmente desde **4.2.8**. 4.2.1 arranca casi siempre, pero hay avisos/roturas menores. | Instalar `Django>=4.2.8,<5` (p. ej. 4.2.20). Misma serie 4.2, sin cambios de código. |
| `django-htmlmin==0.11.0` | Puro Python (html5lib + beautifulsoup4), proyecto abandonado (2019) pero funcional. | Mantener. Plan B si fallara: en `OpenSite/settings.py` quitar las dos líneas `htmlmin.middleware.*` de `MIDDLEWARE` y poner `HTML_MINIFY = False`. |
| `requests` | Sin pin, OK. | Mantener. |
| `scipy==1.11.1` | **Bloqueante**: 1.11.1 no tiene wheels cp312 (los primeros cp312 son de 1.11.2). En Windows pip intentaría compilar desde fuente (meson + fortran) y falla. | Instalar `scipy>=1.11.3` (desplegado con 1.18.0). `OpenBench/stats.py` solo usa APIs estables de `scipy.stats`/`optimize`. |

El **worker** (`Client/requirements.txt`) pide `requests`, `psutil>=5.9.5`, `py-cpuinfo>=9.0.0` — todos compatibles con 3.12 en Windows sin cambios.

Instalación recomendada (sin tocar el requirements.txt del repo):

```powershell
pip install "Django>=4.2.8,<5" django-htmlmin==0.11.0 requests "scipy>=1.11.3"
```

---

## 2. Despliegue paso a paso del servidor

### 2.1 venv + dependencias

```powershell
cd "C:\Users\djime\Documents\Chess_variants\Codex\Fairy-Stockfish organization\openbench-spell"
py -3.12 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install "Django>=4.2.8,<5" django-htmlmin==0.11.0 requests "scipy>=1.11.3"
```

### 2.2 Configuración ANTES de arrancar

Importante: `OpenBench/config.py` carga y valida `Config/config.json`, `Books/*.json` y `Engines/*.json` **al arrancar** (vía `apps.py`) y hace `sys.exit()` si algo no valida. Hay que dejar la configuración lista antes de `runserver`.

1. **`Config/config.json`** — sustituir la lista `"engines"` (hoy `Frolic/Prolix/Stormphranj/Tilted`) por:
   ```json
   "engines": [ "Spell-Stockfish" ]
   ```
   Nota: `client_repo_url`/`client_repo_ref` (hoy `https://github.com/sscg13/OpenBench`, `master`) son de donde los workers se auto-descargan; cuando modifiques el cliente (variante spell), apúntalos a tu propio fork y sube `client_version` en consecuencia.

2. **`Engines/Spell-Stockfish.json`** — ver sección 3 (JSON completo).

3. **`Config/credentials.spell-stockfish`** — fichero de UNA línea con un GitHub PAT con acceso de lectura al repo privado `Belzedar94/Spell-Stockfish` (contents:read + actions:read). El nombre lo impone `read_git_credentials`: `credentials.` + nombre del engine en minúsculas sin espacios. Sin este fichero, un engine con `"private": true` no pasa la validación de arranque (`OpenBench/config.py`, línea 119-121). Ya está en `.gitignore` (`credentials*`).

### 2.3 Migraciones y superusuario

El repo **no incluye** carpeta `OpenBench/migrations/`, así que hay que generarlas:

```powershell
python manage.py makemigrations OpenBench
python manage.py migrate
python manage.py createsuperuser
```

DB por defecto: SQLite en `db.sqlite3` (raíz del repo), definido en `OpenSite/settings.py`.

### 2.4 Arrancar el servidor

```powershell
python manage.py runserver 0.0.0.0:8000
```

Ojo: `runserver` está sobreescrito (`OpenBench/management/commands/runserver.py`) y arranca además dos hilos: `ArtifactWatcher` (poll de artifacts de GitHub Actions para tests "awaiting") y `PGNWatcher`. No uses `--noreload` a ciegas ni gunicorn sin replicar esos watchers.

### 2.5 Configuración inicial en la web

1. **Usuario de trabajo**: ir a `http://localhost:8000/register/` y crear el usuario con el que lanzarás tests y workers (`require_manual_registration` es `false`, así que el formulario está abierto). El `Profile` se crea con `enabled=False, approver=False`.
2. **Habilitarlo**: entrar en `http://localhost:8000/admin/` con el superusuario → modelo *Profiles* → marcar `enabled` y `approver` para tu usuario. Sin `enabled` no puede hacer nada; sin `approver` no puede aprobar tests ni subir redes.
3. **Subir libro**: los libros no se "suben" a la web: se declaran en `Books/*.json` + `Config/config.json` y el worker los descarga de `source` (ver sección 4).
4. **Registrar red (NNUE)**: dos vías, ambas requieren `approver`:
   - Web: `http://localhost:8000/networks/` → formulario de upload (elige engine `Spell-Stockfish`, nombre, fichero).
   - Script: 
     ```powershell
     python Scripts\upload_net.py -U <user> -P <pass> -S http://localhost:8000 -E Spell-Stockfish -N <nombre-red> -F <ruta\red.nnue>
     ```
   La red queda identificada por los primeros 8 hex de su sha256; en los tests se selecciona por nombre y el worker la verifica por sha. Marca una como *default* en /networks/ si quieres que se use automáticamente.
5. **Crear el primer test**: `http://localhost:8000/newTest/` (o botón del engine). El campo *bench* se toma del formulario o se parsea del mensaje del commit con la regex `(?:BENCH|NODES)[ :=]+([0-9,]+)` — disciplina: terminar los commits de `master`/ramas dev con `Bench: <nodos>`.

### 2.6 Arrancar un worker (misma máquina u otra)

```powershell
cd Client
pip install -r requirements.txt
# copia del token para el engine privado, en el cwd del worker:
copy ..\Config\credentials.spell-stockfish .
python client.py -U <user> -P <pass> -S http://localhost:8000 -T <hilos> -N <sockets>
# p. ej. en el 5950X: -T 28 -N 1   (deja margen para el SO)
```

El cliente se auto-actualiza desde `client_repo_url` si `client_version` (32) no coincide. `cutechess-ob.exe` ya viene incluido en `Client/`.

---

## 3. Engines/Spell-Stockfish.json

### 3.1 Campos exigidos (validados en `OpenBench/config.py`)

- `private` : bool
- `nps` : int > 0 (NPS de referencia para escalar TCs entre máquinas; calibrar con un bench real)
- `source` : str (URL del repo GitHub)
- `build` : dict con `cpuflags` (lista str) y `systems` (lista str)
  - si `private == true`: debe existir `Config/credentials.spell-stockfish` (el server NO exige `path`/`compilers`)
  - si `private == false`: además `build.path` (str) y `build.compilers` (lista str)
- `test_presets` / `tune_presets` / `datagen_presets`: opcionales; si faltan se crea `{"default": {}}`. Solo se validan las **claves** de cada preset, no los valores.

### 3.2 Cómo compila/obtiene binarios el worker (clave para decidir `private`)

- **Engine público** (`Client/utils.py::download_public_engine`): descarga el zipball del commit SIN token (falla con repo privado) y compila con `make -j EXE=<out> [CC/CXX=<compiler>] [EVALFILE=<red>]` dentro de `build.path`. **No pasa `ARCH=` ni `COMP=`**: el Makefile de `src` necesitaría un target por defecto tipo shim OpenBench (como `AndyGrant/Stockfish`) que internamente haga `make build ARCH=native`.
- **Engine privado** (`download_private_engine` + `verify_workload.py::fetch_artifact_url`): NO compila en el worker. El server localiza la ejecución de **GitHub Actions del workflow `openbench.yml`** para el commit testeado y el worker descarga el artifact adecuado con su token. Nombres de artifact obligatorios con 4 campos separados por guión: `<tag>-<os>-<vector>-<bitop>` con `os ∈ {windows, linux}` (minúsculas), `vector ∈ {vnni, avx512, avx2, avx, sse4, ssse3}`, `bitop ∈ {pext, popcnt}`.

Como `Belzedar94/Spell-Stockfish` es **privado**, la vía correcta es `"private": true` + workflow de Actions. El `make -j build ARCH=x86-64-bmi2 COMP=mingw` (Windows) / `COMP=gcc` (Linux) vive entonces en `.github/workflows/openbench.yml` del repo del motor, no en este JSON.

Detalle del selector de artifacts (`Client/utils.py::select_best_artifact`): en CPUs cuyo nombre contiene AMD/RYZEN fuerza `has_bmi2 = False`, de modo que en tu 5950X preferirá `popcnt`. Publica artifacts `pext` **y** `popcnt` (el build `x86-64-bmi2` puede etiquetarse `pext`; añade un build `x86-64-avx2`/popcnt).

### 3.3 JSON completo listo para usar

Guardar como `Engines/Spell-Stockfish.json` (los presets solo usan claves válidas; `book_name` apunta al libro spell de la sección 4 — créalo antes de lanzar tests):

```json
{
    "private" : true,
    "nps"     : 800000,
    "source"  : "https://github.com/Belzedar94/Spell-Stockfish",

    "build" : {
        "cpuflags" : ["AVX2", "FMA", "POPCNT"],
        "systems"  : ["Windows", "Linux"]
    },

    "test_presets" : {

        "default" : {
            "base_branch"     : "master",
            "book_name"       : "spell_2moves.epd",
            "test_bounds"     : "[0.00, 2.00]",
            "test_confidence" : "[0.05, 0.05]",
            "win_adj"         : "movecount=5 score=600",
            "draw_adj"        : "movenumber=32 movecount=6 score=15"
        },

        "STC" : {
            "both_options"      : "Threads=1 Hash=16",
            "both_time_control" : "10.0+0.10",
            "workload_size"     : 32
        },

        "LTC" : {
            "both_options"      : "Threads=1 Hash=64",
            "both_time_control" : "60.0+0.6",
            "workload_size"     : 8,
            "test_bounds"       : "[0.50, 2.50]"
        },

        "STC Simplification" : {
            "both_options"      : "Threads=1 Hash=16",
            "both_time_control" : "10.0+0.10",
            "workload_size"     : 32,
            "test_bounds"       : "[-1.75, 0.25]"
        },

        "LTC Simplification" : {
            "both_options"      : "Threads=1 Hash=64",
            "both_time_control" : "60.0+0.6",
            "workload_size"     : 8,
            "test_bounds"       : "[-1.75, 0.25]"
        },

        "STC Regression" : {
            "both_options"      : "Threads=1 Hash=16",
            "both_time_control" : "10.0+0.10",
            "workload_size"     : 32,
            "test_max_games"    : 40000
        },

        "STC Fixed Games" : {
            "both_options"      : "Threads=1 Hash=16",
            "both_time_control" : "10.0+0.10",
            "workload_size"     : 32,
            "test_max_games"    : 20000
        }
    },

    "tune_presets" : {

        "default" : {
            "book_name" : "spell_2moves.epd",
            "win_adj"   : "movecount=5 score=600",
            "draw_adj"  : "movenumber=32 movecount=6 score=15"
        },

        "SPSA STC" : {
            "dev_options"      : "Threads=1 Hash=16",
            "dev_time_control" : "10.0+0.10"
        }
    },

    "datagen_presets" : {

        "default" : {
            "win_adj"       : "None",
            "draw_adj"      : "None",
            "workload_size" : 128
        }
    }
}
```

Notas obligatorias sobre este JSON:

- **Tokens para workers**: con `"private": true`, además de `Config/credentials.spell-stockfish` en el server, **cada worker** necesita un fichero `credentials.spell-stockfish` (una línea con el PAT) en su directorio de trabajo (`Client/`). El server además rechaza tests desde forks del repo (`requests_illegal_fork`): los tests solo pueden apuntar a `https://github.com/Belzedar94/Spell-Stockfish`.
- **Rama base**: `base_branch: "master"` según lo pedido.
- **Bench**: para un engine SIN red el cliente ejecuta **`bench` sin argumentos** (`Client/bench.py:73`); para un engine privado CON red asignada (nuestro caso) ejecuta `['./bin', 'setoption name EvalFile value <red>', 'bench', 'quit']` (`Client/bench.py:76-78`) — la firma que cuenta es la del bench **con la red cargada**. Medido en el 5950X: default embebida 13.456.297 (36 s) · con run5rl **11.477.541** (53 s, 223k NPS) → disciplina de commits: `Bench: 11477541` mientras la red asignada sea run5rl. La firma se parsea de la última línea que case `nodes searched\s+\d+` (case-insensitive) → el `Nodes searched: N` estándar vale tal cual. OJO: el cliente lanza UN BENCH POR HILO del worker EN PARALELO; con el timeout original de 60 s el bench de 53 s moriría — subido a `MAX_BENCH_TIME_SECONDS = 300` en nuestro fork.
- **`nps: 800000` es un placeholder**: mídelo en el 5950X (p. ej. con `Scripts/bench_engine.py` o mirando el NPS que reporta el propio worker) y actualiza el JSON; ese número escala los TCs entre máquinas.
- El `make -j build ARCH=x86-64-bmi2 COMP=mingw` (Windows) / `COMP=gcc` (Linux) debe implementarse en `.github/workflows/openbench.yml` del repo del motor, subiendo artifacts llamados p. ej. `${{ github.sha }}-windows-avx2-pext`, `${{ github.sha }}-windows-avx2-popcnt`, `${{ github.sha }}-linux-avx2-pext`, `${{ github.sha }}-linux-avx2-popcnt` (cada artifact contiene solo el binario). Hay una plantilla mínima en `Documentation/openbench.yml` (basada en Ethereal, hay que adaptarla).

---

## 4. Cómo añade este fork un libro nuevo

Mecanismo (en `OpenBench/config.py::load_book_config` y `Client/utils.py::download_opening_book`):

1. Crear `Books/<nombre>.json`, p. ej. `Books/spell_2moves.epd.json`:
   ```json
   {
       "sha"    : "<sha256 hex del fichero .epd EXTRAÍDO>",
       "source" : "https://.../spell_2moves.epd.zip"
   }
   ```
2. Añadir `"spell_2moves.epd"` a la lista `"books"` de `Config/config.json`.
3. Reiniciar el servidor (la config se carga al arranque).

Detalles críticos:

- `source` debe ser un **ZIP** que contenga el fichero llamado exactamente `spell_2moves.epd`; el worker lo descarga **sin autenticación** (no puede vivir en un repo privado). Opciones: repo público de libros (estilo `AndyGrant/openbench-books` vía raw.githubusercontent), un release público, o un HTTP server local accesible por los workers.
- El `sha` es el sha256 del **contenido de texto del .epd extraído** con newlines universales (`Client/utils.py:255-257` abre en modo TEXTO: CRLF→LF antes de hashear), no del zip ni de los bytes crudos. Cálculo correcto: `python -c "import hashlib;print(hashlib.sha256(open('spell_openings.epd').read().encode('utf-8')).hexdigest())"` — ¡NO usar `'rb'`: con un .epd CRLF el sha binario NO coincide con el que computa el worker! (Nuestro libro es CRLF: texto `bd3296aa...` ≠ binario `ec838062...`.)
- **El nombre del libro decide la variante de cutechess** (`Client/worker.py::Cutechess.basic_settings`): si contiene `SHATRANJ` → `-variant shatranj`; si contiene `FRC`/`960`/`FISCHER` → `fischerandom`; si no → `standard`. Hoy el fork NO conoce ninguna variante "spell": habrá que extender ese mapeo en el cliente y dotar a `cutechess-ob`/`cutechess-ob.exe` de soporte para la variante spell (mismo patrón que usó sscg13 para shatranj).

---

## 5. Riesgos

Ver lista estructurada abajo (sección `risks`). Los tres mayores: (1) el cliente/cutechess no soporta aún la variante spell — sin eso los tests jugarían ajedrez estándar o partidas ilegales; (2) auth de repo privado: PAT en texto plano replicado en server y cada worker, y artifacts de Actions que expiran; (3) `OpenSite/settings.py` trae SECRET_KEY hardcodeada, `DEBUG=True`, `ALLOWED_HOSTS=['*']` y SQLite — aceptable solo en LAN local.

---

## 6. Verificación rápida post-despliegue (checklist)

1. `python manage.py runserver` arranca sin `sys.exit()` de config (si un JSON está mal, lo imprime y sale).
2. `/register/` + `/admin/` → Profile `enabled`+`approver`.
3. `/networks/` acepta subir una red para Spell-Stockfish.
4. Crear un test STC dummy master-vs-master con `Bench:` en el commit → pasa a *awaiting* hasta que Actions termina → un worker con `credentials.spell-stockfish` descarga el artifact, reproduce el bench y empieza a jugar.


## Riesgos (lista estructurada)

- Variante spell no soportada por el cliente: Client/worker.py solo mapea standard/fischerandom/shatranj según el nombre del libro, y cutechess-ob(.exe) incluido no conoce spell chess. Sin portar la variante al cliente y a cutechess, los tests jugarían con reglas estándar. Además, tras modificar el cliente hay que apuntar client_repo_url/client_repo_ref de Config/config.json al fork propio y ajustar client_version, o los workers se auto-actualizarán desde sscg13/OpenBench y perderán los cambios.
- Repo privado: con private:true el PAT vive en texto plano en Config/credentials.spell-stockfish (server) y en credentials.spell-stockfish de CADA worker. Usar un fine-grained PAT de solo lectura (contents:read + actions:read) limitado a Belzedar94/Spell-Stockfish. .gitignore ya excluye credentials*, pero cuidado con copias en backups/scratch.
- Artifacts de GitHub Actions expiran (90 días por defecto y el server exige not expired en fetch_artifact_url): re-testear commits antiguos fallará dejando el test en awaiting hasta relanzar el workflow. Considerar retention-days explícito en openbench.yml.
- SECRET_KEY hardcodeada en OpenSite/settings.py + DEBUG=True + ALLOWED_HOSTS=['*']: solo aceptable en red local; si se expone el server (aunque sea a workers remotos por internet), rotar SECRET_KEY vía variable de entorno, DEBUG=False y restringir ALLOWED_HOSTS.
- DB por defecto SQLite (db.sqlite3): con varios workers reportando concurrentemente puede dar 'database is locked'. Suficiente para 1-3 workers locales; migrar a PostgreSQL si crece la flota. Hacer backup del fichero antes de migraciones.
- Pin scipy==1.11.1 no instala en Python 3.12 (sin wheels cp312; en Windows intenta compilar y falla). Usar scipy>=1.11.3. Django 4.2.1 tampoco soporta 3.12 oficialmente: usar >=4.2.8,<5. django-htmlmin está abandonado: si rompe, retirar su middleware.
- Bench: el worker exige determinismo entre hilos y coincidencia exacta con el bench declarado; con red asignada el bench corre con `setoption EvalFile` previo (firma = 11.477.541 con run5rl). Los benches corren en paralelo (uno por hilo): timeout del fork subido a 300 s.
- Selección de artifacts en Ryzen: select_best_artifact fuerza has_bmi2=False en CPUs AMD/Ryzen, así que el 5950X PREFIERE popcnt; si solo existieran artifacts pext, el fallback `options[artifacts[0]]` usaría el pext igualmente (funciona, pero con pext lento de Zen3 en movegen crítico). Publicar ambos: pext y popcnt.
- Los libros se descargan sin autenticación desde book.source: deben alojarse en URL pública (repo/release público o HTTP local accesible), con sha256 del .epd extraído exacto o el worker borra la descarga y falla.
- El repo no trae migraciones de la app OpenBench: hay que ejecutar makemigrations OpenBench antes de migrate; saltárselo deja la DB sin tablas del modelo (Profile, Test, Machine, Network...).


## Próximas acciones

- Crear el venv 3.12 e instalar deps ajustadas: pip install "Django>=4.2.8,<5" django-htmlmin==0.11.0 requests "scipy>=1.11.3"; luego makemigrations OpenBench + migrate + createsuperuser.
- Añadir en Spell-Stockfish (repo del motor) el workflow .github/workflows/openbench.yml que compile con make -j build ARCH=x86-64-bmi2 COMP=mingw (windows-latest) y COMP=gcc (ubuntu-latest) y suba artifacts <sha>-{windows,linux}-avx2-{pext,popcnt} con el binario.
- Verificar/ajustar que 'bench' sin argumentos en Spell-Stockfish equivale a 'bench 16 1 10 default depth' y que imprime 'Nodes searched:'; adoptar la disciplina de 'Bench: N' al final de los commits.
- Crear Engines/Spell-Stockfish.json (JSON de la sección 3.3), Config/credentials.spell-stockfish (PAT fine-grained de solo lectura) y editar Config/config.json (engines=["Spell-Stockfish"]).
- Generar el libro de aperturas spell (spell_2moves.epd), empaquetarlo en zip, publicarlo en URL pública, calcular sha256 del .epd y crear Books/spell_2moves.epd.json + entrada en config.json.
- Portar la variante spell al cliente: extender Cutechess.basic_settings en Client/worker.py (mapeo nombre-de-libro→variante, patrón shatranj) y compilar un cutechess-ob(.exe) con soporte spell; después apuntar client_repo_url/ref a vuestro fork y subir client_version.
- Medir el NPS real de referencia en el 5950X (Scripts/bench_engine.py o NPS reportado por el worker) y actualizar el campo nps del JSON.
- Prueba de humo end-to-end: runserver, registrar usuario y habilitarlo (enabled+approver) en /admin, subir una red por /networks o Scripts/upload_net.py, lanzar un test STC master-vs-master y conectar un worker local con credentials.spell-stockfish en Client/.

---

## Estado tras el despliegue + ronda de verificación adversarial (2026-07-12)

Completado (el cuerpo de arriba se conserva como referencia; estos puntos ya no están pendientes):
- venv 3.12 + deps ✅ (Django 4.2.30, scipy 1.18.0) · makemigrations+migrate ✅ · superusuario `belzedar` con Profile enabled+approver ✅ · runserver HTTP 200 ✅ · red run5rl subida por SHA vía `Scripts/upload_net.py` ✅.
- `Config/config.json`: engines=["Spell-Stockfish"], libro `spell_openings.epd` añadido, `client_repo_url→https://github.com/Belzedar94/OpenBench` + `client_repo_ref→spell-runner` (pendiente: publicar el fork en GitHub antes de instalar workers limpios remotos).
- `Engines/Spell-Stockfish.json` con nps=380000 y presets sobre `spell_openings.epd`; `Books/spell_openings.epd.json` con el sha de TEXTO (`bd3296aa...`); `Config/credentials.spell-stockfish` = PLACEHOLDER (sustituir por PAT real).
- La sección 4 quedó obsoleta en un punto: el mapeo variante→runner YA está implementado (tabla `VARIANTS` + fallback `ENGINE_VARIANTS` por nombre de engine para DATAGEN con book='None', rama `spell-runner`); no hace falta portar spell a cutechess-ob — SPELL rutea al `uci_pair_runner.py`.

Fixes derivados de la verificación adversarial (rama `spell-runner` del fork):
- `Client/pgn_util.py`: REGEX_MOVE_AND_COMMENT ahora admite `@` y `,` — sin esto la subida de PGNs truncaba cada gated move (`f@e4,d2d4`→`d2d4`).
- `Client/bench.py`: `MAX_BENCH_TIME_SECONDS` 60→300 (bench con run5rl = 53 s en solitario y corre uno-por-hilo en paralelo).
- `Client/worker.py`: SPELL primero en la tabla (gana a FRC/960 en nombres combinados); short-path 8.3 para `sys.executable` con espacios (nuestro venv vive bajo "Fairy-Stockfish organization").
- Pendientes conocidos del flujo de aborto: `kill_everything` no mata al runner por nombre — el runner se autoprotege (pipe-muerto → exit duro matando motores; circuit breaker tras 3 muertes instantáneas consecutivas), y al empaquetar para workers remotos debe llamarse `cutechess-ob` (pyinstaller) para entrar en el kill-by-name.
- Gotcha del auto-update del cliente (`Client/client.py:146`): el zip descargado debe extraer una carpeta raíz llamada exactamente `OpenBench-<ref>` — al publicar el fork, mantener el nombre de repo `OpenBench`.
- Matiz de validación al arranque: `verify_general_config` es un no-op (bug de paréntesis `assert type(x == int)` upstream); la validación real que aborta es la de `Engines/*.json`/`Books/*.json`/credentials. No confiar en que el server valide claves de `Config/config.json`.
