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

1. The Windows `dontstarve_steam.exe` (x86-32 PE, VC9) is disassembled and lifted to C99.
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
through the CRT startup into the game's own `main()`, up to its Steam check, and exits cleanly. The map
fills in as the port progresses (measured with a `COVERAGE=1` build, see `scripts/coverage_counts.py`).

![progress map](docs/progress-en.svg)

![milestones](docs/milestones-en.svg)

| Milestone | Status |
|---|---|
| M0 Input decision + toolchain | ✅ done |
| M1 Function catalog — 17,033 functions | ✅ done |
| M2 Lift to C99 — 4.2M lines, **0 failures** | ✅ done |
| M3 Compiles + links (exe + 2 lifted CRT DLLs) | ✅ done |
| M4 Arena host, IAT binding (955/955), CRT `DllMain` runs | ✅ done |
| M5 CRT initialization — both CRT `DllMain`s + exe CRT startup, game `main()` runs | ✅ done |
| **M6 Native main menu** | 🟡 **in progress — USER32/GDI32 headless window done; blocked on the Steam check** |
| M7 Stability | ⬜ |
| M8 Audio (FMOD Ex shim) | ⬜ |
| M9 PortMaster package + FPS vs box64 | ⬜ |

### 🙋 Where help is needed

**Current wall: the Steam check, not a missing import.** USER32 + GDI32 now have a headless
shim layer ([src/runtime/ds_user32.c](src/runtime/ds_user32.c)): one fixed 640×480 window (the
R36S screen), a single display mode and monitor, an empty message queue, and the guest `WndProc`
driven for `WM_NCCREATE`/`WM_CREATE`/`WM_SIZE`/`WM_SHOWWINDOW`/`WM_ACTIVATE`. With it the game runs
further, **914 functions executed (520 in the game exe)**, then calls `SteamAPI_Init()`. Our
`steam_api` shim honestly answers "not running under Steam", so the Steam build shows its own
*"Steam must be running to play this game"* box and exits cleanly (static destructors and
`ExitProcess` included). This exe has no offline path: it checks before it creates its window.

This project **will not fake Steam ownership or licences**. Ways forward we'd like input on:

- **A DRM-free build of the game.** The pipeline doesn't depend on any one exe, so a standalone
  `dontstarve.exe` from your own purchase would be re-lifted the same way.
- Meanwhile, the next layers can be built and tested on their own: the **libEGL/libGLESv2**
  bridge (ANGLE, imported by ordinal) to native GLES2, **SDL2** behind the fake window
  (KMSDRM on the device), the **FMOD Ex + FMOD Event** shim, **XINPUT/DINPUT8** → SDL2.

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

Requirements: Windows + WSL (gcc), Python 3.10 with `capstone pefile unicorn keystone-engine`.

```bash
# 1. pcrecomp, pinned + patched (patches from nfsu2-recompiled-r36s)
git clone https://github.com/sp00nznet/pcrecomp && cd pcrecomp
git checkout 35548b8 && git am ../nfsu2-recompiled-r36s/patches/pcrecomp/*.patch
git am ../patches/pcrecomp/*.patch && cd ..   # this repo's own lifter fixes (0015+)

# 2. Put YOUR dontstarve_steam.exe in work/, and msvcr90.dll + msvcp90.dll
#    (x86, 9.0.30729.x — from your Windows WinSxS or the game's vcredist)

# 3. Lift (Windows Python) — see scripts/
python scripts/run_lift.py catalog work/dontstarve_steam.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift    work/dontstarve_steam.exe work/catalog.json src/recomp/gen
bash   scripts/lift_dll.sh work/msvcr90.dll msvcr90_
bash   scripts/lift_dll.sh work/msvcp90.dll msvcp90_
python scripts/find_data_funcs.py work/msvcr90.dll work/msvcr90_catalog.json config/exclude.txt
python scripts/find_data_funcs.py work/msvcp90.dll work/msvcp90_catalog.json config/exclude.txt
#    then re-run the two DLL lifts so the exclude list applies (lift_dll.sh also adds reloc seeds)

# 4. Build + run the host (WSL)
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

1. O `dontstarve_steam.exe` de Windows (x86-32 PE, VC9) é desmontado e liftado para C99.
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
startup do CRT, entra no `main()` do jogo, vai até a checagem da Steam e sai de forma limpa. O mapa
vai se preenchendo conforme o port avança (medido com um build `COVERAGE=1`, veja `scripts/coverage_counts.py`).

![mapa de progresso](docs/progress-pt.svg)

![marcos](docs/milestones-pt.svg)

| Marco | Status |
|---|---|
| M0 Decisão de input + toolchain | ✅ feito |
| M1 Catálogo de funções — 17.033 funções | ✅ feito |
| M2 Lift para C99 — 4,2M linhas, **0 falhas** | ✅ feito |
| M3 Compila + linka (exe + 2 DLLs do CRT liftadas) | ✅ feito |
| M4 Host arena, bind da IAT (955/955), `DllMain` do CRT roda | ✅ feito |
| M5 Inicialização do CRT — `DllMain` dos 2 CRTs + startup do exe, `main()` do jogo roda | ✅ feito |
| **M6 Menu principal nativo** | 🟡 **em progresso — janela headless USER32/GDI32 pronta; travado na checagem da Steam** |
| M7 Estabilidade | ⬜ |
| M8 Áudio (shim do FMOD Ex) | ⬜ |
| M9 Pacote PortMaster + FPS vs box64 | ⬜ |

### 🙋 Onde precisamos de ajuda

**Parede atual: a checagem da Steam, não um import faltando.** USER32 + GDI32 agora têm uma
camada de shims headless ([src/runtime/ds_user32.c](src/runtime/ds_user32.c)): uma janela fixa de
640×480 (a tela do R36S), um único modo de vídeo e monitor, fila de mensagens vazia, e o `WndProc`
do jogo recebendo `WM_NCCREATE`/`WM_CREATE`/`WM_SIZE`/`WM_SHOWWINDOW`/`WM_ACTIVATE`. Com isso o jogo
avança mais, **914 funções executadas (520 no exe do jogo)**, e chama `SteamAPI_Init()`. Nosso shim
de `steam_api` responde com honestidade "não está rodando sob a Steam", então o build Steam mostra a
própria caixa *"Steam must be running to play this game"* e sai de forma limpa (incluindo os
destrutores estáticos e o `ExitProcess`). Esse exe não tem caminho offline: a checagem vem antes de
criar a janela.

Este projeto **não vai falsificar posse nem licença da Steam**. Caminhos em que queremos opinião:

- **Um build DRM-free do jogo.** O pipeline não depende de um exe específico, então um
  `dontstarve.exe` standalone da sua própria compra seria liftado do mesmo jeito.
- Enquanto isso, as próximas camadas dá para construir e testar sozinhas: a ponte
  **libEGL/libGLESv2** (ANGLE, importados por ordinal) para GLES2 nativo, **SDL2** por trás da
  janela falsa (KMSDRM no aparelho), o shim **FMOD Ex + FMOD Event**, **XINPUT/DINPUT8** → SDL2.

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

Requisitos: Windows + WSL (gcc), Python 3.10 com `capstone pefile unicorn keystone-engine`.
Os comandos são os mesmos da seção em inglês acima.

---

## Repository layout / Estrutura

```
docs/          design decisions, architecture, status page (index.html)
scripts/       lift + build drivers (run_lift.py, lift_dll.sh, find_data_funcs.py, build_*.sh, gen_status.py)
src/runtime/   arena host (ds_host.c), shim ABI (ds_hle.h), kernel32/CRT shims (ds_kernel32.c),
               platform shims: winmm, steam_api, COM, shell32 (ds_win32.c),
               USER32 + GDI32 headless window (ds_user32.c)
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
