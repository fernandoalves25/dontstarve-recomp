# Don't Starve — Native ARM64 Recompilation for R36S

**[English](#english) · [Português](#português)** · 📊 **[Live status dashboard](https://fernandoalves25.github.io/dontstarve-recomp/)**

![executed at runtime](docs/badge-executed-en.svg)
![functions lifted](docs/badge-functions.svg)
![lift failures](docs/badge-failures.svg)
![imports](docs/badge-imports.svg)
![phase](docs/badge-phase-en.svg)
![Platform](https://img.shields.io/badge/target-R36S%20%2F%20RK3326-blue)
![License](https://img.shields.io/badge/tooling-MIT-lightgrey)

> ⚠️ **No game files are included.** This repo holds only original tooling and runtime.
> You need your own legally obtained copy of Don't Starve. / **Nenhum arquivo do jogo é incluído.**

---

## English

Static recompilation of **Don't Starve** (PC, single-player) into **native ARM64 C** for the
R36S handheld (Rockchip RK3326, 4× Cortex-A35, ArkOS / PortMaster) — **no box64, no runtime
emulation**. Built on [pcrecomp](https://github.com/sp00nznet/pcrecomp), following the approach
proven by [nfsu2-recompiled-r36s](https://github.com/manuelpllull/nfsu2-recompiled-r36s).

**Why:** the current box64 port tops out around **20–22 FPS**, CPU-bound on x86→ARM translation
overhead over the Lua VM. Native code removes that overhead — while keeping everything the
Android port loses: all DLCs (RoG, Shipwrecked, Hamlet), **mods**, the **PT-BR translation**
and the PC UI.

### How it works

1. The Windows **DRM-free standalone** `dontstarve.exe` (x86-32 PE, VC9) is disassembled and lifted to C99.
   *(pcrecomp has no ELF front-end — its "64-bit path" is Win64 PE — so the Linux build is not usable.)*
2. The **VC9 C/C++ runtime** (`msvcr90.dll`, `msvcp90.dll`) is **lifted too**, so the STL and
   CRT run as guest code with their exact ABI — no hand-written `std::string` shims.
3. A custom **64-bit arena host** runs it all: the guest lives in a contiguous arena, guest
   pointers stay 32-bit, every access is `arena_base + va`. The same model ships on aarch64.
4. Only the real OS boundary is HLE (kernel32 / user32 / gdi32 …). Graphics are already GLES2
   (ANGLE), so they bridge straight to the Mali GPU — no D3D9 translation layer needed.

### Status

**How much of the game actually runs.** Every function is already *lifted* to C — that's the
easy part. What matters is how many have **executed** natively without crashing. Each cell ≈ 16
functions: **bright = ran at runtime**, dim = lifted but never reached yet. Today execution runs
through the CRT startup and the game's own init, creates the game window and a real GLES2 context,
and stops at the first FMOD (audio) call. The map
fills in as the port progresses (measured with a `COVERAGE=1` build, see `scripts/coverage_counts.py`).

![progress map](docs/progress-en.svg)

![milestones](docs/milestones-en.svg)

| Milestone | Status |
|---|---|
| M0 Input decision + toolchain | ✅ done |
| M1 Function catalog — 16,146 functions (standalone exe) | ✅ done |
| M2 Lift to C99 — 4.2M lines, **0 failures** | ✅ done |
| M3 Compiles + links (exe + 2 lifted CRT DLLs) | ✅ done |
| M4 Arena host, IAT binding (955/955), CRT `DllMain` runs | ✅ done |
| M5 CRT initialization — both CRT `DllMain`s + exe CRT startup, game `main()` runs | ✅ done |
| **M6 Native main menu** | 🟡 **in progress — window + EGL/GLES2 bridge done; FMOD (silent) next** |
| M7 Stability | ⬜ |
| M8 Audio (FMOD Ex shim) | ⬜ |
| M9 PortMaster package + FPS vs box64 | ⬜ |

### 🙋 Where help is needed

**Current wall: `FMOD_EventSystem_Create`, the first audio call.** The port lifts the game's
**DRM-free standalone build** (`dontstarve.exe`, 16,146 functions, 0 lift failures); the Steam one
won't start without a running Steam client. Execution reaches **1,335 functions (787 in the game
exe)**. The game writes its own `log.txt`, probes its DLC/patch folders and settings, creates its
`"opengles2.0"` window, brings up **EGL + GLES2 for real** (on the host's native driver, Mesa
llvmpipe in CI-style headless runs), queries the GL texture formats, enumerates input devices,
reaches `cDontStarveGame::DoGameSpecificInitialize()` and creates the FMOD event system.

What's in the runtime now ([src/runtime](src/runtime)):

- **EGL/GLES2 bridge** ([ds_gles.c](src/runtime/ds_gles.c)): the 9 EGL + 76 GLES2 ANGLE imports
  (by ordinal, mapped from the game's own `libEGL.dll`/`libGLESv2.dll` export tables) straight onto
  native EGL/GLES2. Headless for now: a 640×480 pbuffer on `EGL_PLATFORM_SURFACELESS_MESA`;
  `DS_DUMP_FRAMES=N` saves every Nth frame to `build/frames/`. The R36S build puts an SDL2/KMSDRM
  surface on the Mali GPU in its place.
- **USER32 + GDI32** ([ds_user32.c](src/runtime/ds_user32.c)): a headless fake window, one fixed
  640×480 client rect (the R36S screen), one display mode and monitor, an empty message queue.
- **File I/O** ([ds_file.c](src/runtime/ds_file.c)): the guest's `Z:\dontstarve\…` maps to
  `$DS_GAME_DIR` (default `gamedata/`: your install's `bin/` and `data/`), `Z:\home\…` to
  `$DS_HOME_DIR` (default `build/home/`: saves, `log.txt`). Lookups are case-insensitive, as on
  Windows. `DS_FILE_TRACE=1` logs every open.
- **Threads** ([ds_thread.c](src/runtime/ds_thread.c)): real host threads. The machine lock is
  released around blocking calls, and threads, mutexes and semaphores are real waitable objects.
- **Input** ([ds_input.c](src/runtime/ds_input.c)): XInput reports no pad; DirectInput 8 is a COM
  interface with no devices (the game calls it without checking the create result).
- **Offline Winsock** ([ds_net.c](src/runtime/ds_net.c)): `WSAStartup` works, but no socket or
  name lookup succeeds.
- **Lifter:** patch [0016](patches/pcrecomp) adds the x87 `fcomi`/`fucomi[p]` compares (3,835
  sites in the exe that lifted to `RECOMP_UNIMPL` because Capstone spells them `fcompi`/`fucompi`),
  with their real EFLAGS (NaN included). pcrecomp's `difftest` matches hardware on 263/263 cases.

Help wanted, in order:

- **FMOD Ex + FMOD Event shim** (21 + 24 imports, C API and stdcall C++ methods): a silent
  bring-up first (valid handles, no sound), then a real backend (SDL audio, FSB banks).
- **SDL2/KMSDRM** behind the window and the EGL surface, SDL events → the message queue, and
  SDL GameController → XInput for the R36S buttons.

Runtime fixes on the way (useful for anyone using pcrecomp): import-stub names were dangling
pointers (`GetProcAddress` stack buffers and ordinal names like `OLEAUT32.dll#9`), so a missing
import reported an empty or wrong name. `HeapSize` returned 0, so the CRT's `_msize`-sized onexit
table walked into string data at exit. Heap blocks now carry a size header.

How the CRT wall fell (for anyone hitting similar walls with pcrecomp): kernel32
`DecodePointer` shared a cdecl shim and didn't pop its argument, shifting `pop esi/pop ebp` in
the CRT's `_decode_pointer` — the `0xDEAD0000` `this`. Found by recompiling one lifted file with
`-g` and printing the lifted locals (`l_esp`, `l_esi`…) after every call under gdb. Then lifter
support for `lock xadd` and `rep ret` ([patches/pcrecomp](patches/pcrecomp)), then
relocation-based seeds ([scripts/reloc_seeds.py](scripts/reloc_seeds.py)) for CRT init tables
that recursive descent never reaches. `DS_SENTINEL=1` (COVERAGE build) aborts on the first
function entered with the sentinel in `esp`/`ebp`.

Ideas, PRs and discussion very welcome — see [docs/01-ARCHITECTURE.md](docs/01-ARCHITECTURE.md).

### Reproducing (you need your own game copy)

Requirements: Linux (or Windows + WSL) with gcc, EGL + GLES2 dev libraries (Mesa: `libegl-dev libgles-dev`),
Python 3.10+ with `capstone pefile unicorn keystone-engine`.

```bash
# 1. pcrecomp, pinned + patched (patches from nfsu2-recompiled-r36s)
git clone https://github.com/sp00nznet/pcrecomp && cd pcrecomp
git checkout 35548b8 && git am ../nfsu2-recompiled-r36s/patches/pcrecomp/*.patch
git am ../patches/pcrecomp/*.patch && cd ..   # this repo's own lifter fixes (0015+)

# 2. Put YOUR DRM-free dontstarve.exe (the standalone build, from bin/) in work/, plus
#    msvcr90.dll + msvcp90.dll (x86, 9.0.30729.x, from Windows WinSxS or a vcredist).
#    The Steam build (dontstarve_steam.exe) needs a running Steam client and won't start.

# 3. Lift — see scripts/
python pcrecomp/tools/cpp/vtable_scan.py work/dontstarve.exe --seeds work/vtable_seeds.json
python pcrecomp/tools/cpp/rtti.py        work/dontstarve.exe --seeds work/rtti_seeds.json
#    merge both into work/seeds_all.json (a JSON list, dedup by "address")
python scripts/run_lift.py catalog work/dontstarve.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift    work/dontstarve.exe work/catalog.json src/recomp/gen
bash   scripts/lift_dll.sh work/msvcr90.dll msvcr90_
bash   scripts/lift_dll.sh work/msvcp90.dll msvcp90_
python scripts/find_data_funcs.py work/msvcr90.dll work/msvcr90_catalog.json config/exclude.txt
python scripts/find_data_funcs.py work/msvcp90.dll work/msvcp90_catalog.json config/exclude.txt
#    then re-run the two DLL lifts so the exclude list applies (lift_dll.sh also adds reloc seeds)

# 4. Build + run the host (game data: DS_GAME_DIR=<your install dir>, default gamedata/)
bash scripts/build_guest_obj.sh && bash scripts/build_arena_host.sh
./build/dontstarve_host --run-entry
```

---

## Português

Recompilação estática do **Don't Starve** (PC, single-player) para **C nativo ARM64** no
portátil R36S (Rockchip RK3326, 4× Cortex-A35, ArkOS / PortMaster) — **sem box64, sem emulação
em runtime**. Baseado no [pcrecomp](https://github.com/sp00nznet/pcrecomp), seguindo o caminho
provado pelo [nfsu2-recompiled-r36s](https://github.com/manuelpllull/nfsu2-recompiled-r36s).

**Por quê:** o port atual via box64 trava em torno de **20–22 FPS**, limitado pela CPU no
overhead da tradução x86→ARM sobre a VM Lua. Código nativo elimina esse overhead — mantendo
tudo que o port Android perde: todas as DLCs (RoG, Shipwrecked, Hamlet), **mods**, a
**tradução PT-BR** e a UI de PC.

### Como funciona

1. O `dontstarve.exe` **standalone DRM-free** de Windows (x86-32 PE, VC9) é desmontado e liftado para C99.
   *(o pcrecomp não tem front-end ELF — o "caminho 64-bit" dele é Win64 PE — então a build Linux não serve.)*
2. O **runtime C/C++ do VC9** (`msvcr90.dll`, `msvcp90.dll`) **também é liftado**, então a STL e
   o CRT rodam como código guest com o ABI exato — sem shims de `std::string` escritos à mão.
3. Um **host arena 64-bit** próprio roda tudo: o guest vive numa arena contígua, ponteiros do
   guest continuam 32-bit, todo acesso é `arena_base + va`. O mesmo modelo vai pro aarch64.
4. Só a fronteira real de SO é HLE (kernel32 / user32 / gdi32 …). Os gráficos já são GLES2
   (ANGLE), então ligam direto na GPU Mali — sem camada de tradução D3D9.

### Status

![executadas em runtime](docs/badge-executed-pt.svg)
![fase](docs/badge-phase-pt.svg)

**Quanto do jogo realmente roda.** Todas as funções já estão *liftadas* para C — essa é a parte
fácil. O que importa é quantas já **executaram** nativamente sem crashar. Cada célula ≈ 16 funções:
**clara = rodou em runtime**, escura = liftada mas ainda não alcançada. Hoje a execução passa pelo
startup do CRT e pela inicialização do jogo, cria a janela do jogo e um contexto GLES2 de verdade, e para na primeira chamada ao FMOD (áudio). O mapa
vai se preenchendo conforme o port avança (medido com um build `COVERAGE=1`, veja `scripts/coverage_counts.py`).

![mapa de progresso](docs/progress-pt.svg)

![marcos](docs/milestones-pt.svg)

| Marco | Status |
|---|---|
| M0 Decisão de input + toolchain | ✅ feito |
| M1 Catálogo de funções — 16.146 funções (exe standalone) | ✅ feito |
| M2 Lift para C99 — 4,2M linhas, **0 falhas** | ✅ feito |
| M3 Compila + linka (exe + 2 DLLs do CRT liftadas) | ✅ feito |
| M4 Host arena, bind da IAT (955/955), `DllMain` do CRT roda | ✅ feito |
| M5 Inicialização do CRT — `DllMain` dos 2 CRTs + startup do exe, `main()` do jogo roda | ✅ feito |
| **M6 Menu principal nativo** | 🟡 **em progresso — janela + ponte EGL/GLES2 prontas; FMOD (mudo) a seguir** |
| M7 Estabilidade | ⬜ |
| M8 Áudio (shim do FMOD Ex) | ⬜ |
| M9 Pacote PortMaster + FPS vs box64 | ⬜ |

### 🙋 Onde precisamos de ajuda

**Parede atual: `FMOD_EventSystem_Create`, a primeira chamada de áudio.** O port lifta o **build
standalone DRM-free** do jogo (`dontstarve.exe`, 16.146 funções, 0 falhas de lift); o build Steam
não inicia sem o cliente da Steam rodando. A execução chega a **1.335 funções (787 no exe do
jogo)**. O jogo escreve o próprio `log.txt`, procura as pastas de DLC/patch e as configurações,
cria a janela `"opengles2.0"`, sobe **EGL + GLES2 de verdade** (no driver nativo do host; Mesa
llvmpipe nos testes headless), consulta os formatos de textura do GL, enumera os dispositivos de
input, chega ao `cDontStarveGame::DoGameSpecificInitialize()` e cria o event system do FMOD.

O que já existe no runtime ([src/runtime](src/runtime)):

- **Ponte EGL/GLES2** ([ds_gles.c](src/runtime/ds_gles.c)): os 9 imports EGL + 76 GLES2 do ANGLE
  (por ordinal, mapeados a partir das tabelas de export do `libEGL.dll`/`libGLESv2.dll` do próprio
  jogo) direto para EGL/GLES2 nativo. Por enquanto headless: um pbuffer de 640×480 em
  `EGL_PLATFORM_SURFACELESS_MESA`; `DS_DUMP_FRAMES=N` salva cada N-ésimo frame em `build/frames/`.
  No R36S entra uma superfície SDL2/KMSDRM na GPU Mali no lugar.
- **USER32 + GDI32** ([ds_user32.c](src/runtime/ds_user32.c)): uma janela falsa headless, um
  retângulo cliente fixo de 640×480 (a tela do R36S), um modo de vídeo e um monitor, fila de
  mensagens vazia.
- **I/O de arquivos** ([ds_file.c](src/runtime/ds_file.c)): o `Z:\dontstarve\…` do guest vira
  `$DS_GAME_DIR` (padrão `gamedata/`: o `bin/` e o `data/` da sua instalação), e `Z:\home\…` vira
  `$DS_HOME_DIR` (padrão `build/home/`: saves, `log.txt`). A busca ignora maiúsculas/minúsculas,
  como no Windows. `DS_FILE_TRACE=1` registra cada abertura.
- **Threads** ([ds_thread.c](src/runtime/ds_thread.c)): threads reais do host. A trava da
  máquina é liberada nas chamadas que bloqueiam, e threads, mutexes e semáforos são objetos de
  espera de verdade.
- **Input** ([ds_input.c](src/runtime/ds_input.c)): o XInput responde que não há controle; o
  DirectInput 8 é uma interface COM sem dispositivos (o jogo a usa sem checar o resultado).
- **Winsock offline** ([ds_net.c](src/runtime/ds_net.c)): o `WSAStartup` funciona, mas nenhum
  socket ou resolução de nome dá certo.
- **Lifter:** o patch [0016](patches/pcrecomp) adiciona as comparações x87 `fcomi`/`fucomi[p]`
  (3.835 pontos no exe que viravam `RECOMP_UNIMPL` porque o Capstone as chama de
  `fcompi`/`fucompi`), com os EFLAGS reais (NaN incluído). O `difftest` do pcrecomp bate com o
  hardware em 263/263 casos.

Procura-se ajuda, nesta ordem:

- **Shim do FMOD Ex + FMOD Event** (21 + 24 imports, API C e métodos C++ stdcall): primeiro sem
  som (handles válidos, nada toca), depois um backend real (áudio SDL, bancos FSB).
- **SDL2/KMSDRM** por trás da janela e da superfície EGL, eventos SDL → fila de mensagens, e SDL
  GameController → XInput para os botões do R36S.

Correções de runtime no caminho (úteis para quem usa o pcrecomp): os nomes dos stubs de import
eram ponteiros pendurados (buffers de pilha do `GetProcAddress` e nomes por ordinal como
`OLEAUT32.dll#9`), então um import faltando aparecia com nome vazio ou errado. O `HeapSize`
retornava 0, e a tabela onexit do CRT (dimensionada por `_msize`) andava por dados de string na
saída. Agora os blocos do heap têm um cabeçalho com o tamanho.

Como a parede do CRT caiu (para quem bater em paredes parecidas com o pcrecomp): o
`DecodePointer` do kernel32 usava um shim cdecl e não desempilhava o argumento, deslocando o
`pop esi/pop ebp` no `_decode_pointer` do CRT — o `this` = `0xDEAD0000`. Achado recompilando um
arquivo liftado com `-g` e imprimindo os locais liftados (`l_esp`, `l_esi`…) depois de cada call
no gdb. Depois, suporte no lifter a `lock xadd` e `rep ret` ([patches/pcrecomp](patches/pcrecomp)),
e seeds por relocação ([scripts/reloc_seeds.py](scripts/reloc_seeds.py)) para as tabelas de init do
CRT que a descida recursiva nunca alcança. `DS_SENTINEL=1` (build COVERAGE) aborta na primeira
função que entra com o sentinela em `esp`/`ebp`.

Ideias, PRs e discussão são muito bem-vindos — veja [docs/01-ARCHITECTURE.md](docs/01-ARCHITECTURE.md).

### Reproduzindo (precisa da sua cópia do jogo)

Requisitos: Linux (ou Windows + WSL) com gcc, bibliotecas dev de EGL + GLES2 (Mesa: `libegl-dev libgles-dev`),
Python 3.10+ com `capstone pefile unicorn keystone-engine`.
Use o seu `dontstarve.exe` **standalone DRM-free** (o build Steam exige o cliente da Steam rodando).
Os comandos são os mesmos da seção em inglês acima.

---

## Repository layout / Estrutura

```
docs/          design decisions, architecture, status page (index.html)
scripts/       lift + build drivers (run_lift.py, lift_dll.sh, find_data_funcs.py, build_*.sh, gen_status.py)
src/runtime/   arena host (ds_host.c), shim ABI (ds_hle.h), kernel32/CRT shims (ds_kernel32.c),
               platform shims: winmm, steam_api, COM, shell32 (ds_win32.c),
               USER32 + GDI32 headless window (ds_user32.c), file I/O (ds_file.c),
               threads + waitable objects (ds_thread.c), offline Winsock (ds_net.c),
               EGL/GLES2 bridge (ds_gles.c), XInput + DirectInput8 (ds_input.c)
patches/       this repo's pcrecomp lifter fixes, applied on top of a847ef6
config/        overrides.txt (native HLE of CRT EH helpers), hooks.txt
```

Not in git (generated locally from *your* game copy) / Fora do git (gerado localmente):
`work/`, `src/recomp/gen/`, `build/`, `pcrecomp/`, `config/exclude.txt`.

## Legal

This repository contains **only original tooling and runtime code** (MIT, see [LICENSE](LICENSE)).
**No Don't Starve binaries, assets or code — lifted or otherwise — are distributed.** All
game-derived material is generated locally from your own copy and is git-ignored.
Don't Starve is © Klei Entertainment. Not affiliated with or endorsed by Klei.

Este repositório contém **apenas ferramentas e runtime originais** (MIT). **Nenhum binário,
asset ou código do Don't Starve — liftado ou não — é distribuído.** Todo material derivado do
jogo é gerado localmente a partir da sua cópia e fica fora do git.
