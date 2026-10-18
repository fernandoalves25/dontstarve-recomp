# Don't Starve — Native ARM64 Recompilation for R36S

**[English](#english) · [Português](#português)** · 📊 **[Live status dashboard](docs/index.html)**

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

**Lift coverage by module** (each cell ≈ 16 functions; gray = data pruned from `.text`, never called):

![progress map](docs/progress-en.svg)

![milestones](docs/milestones-en.svg)

| Milestone | Status |
|---|---|
| M0 Input decision + toolchain | ✅ done |
| M1 Function catalog — 17,033 functions | ✅ done |
| M2 Lift to C99 — 4.2M lines, **0 failures** | ✅ done |
| M3 Compiles + links (exe + 2 lifted CRT DLLs) | ✅ done |
| M4 Arena host, IAT binding (955/955), CRT `DllMain` runs | ✅ done |
| **M5 CRT initialization** | 🟡 **in progress — current wall** |
| M6 Native main menu | ⬜ |
| M7 Stability | ⬜ |
| M8 Audio (FMOD Ex shim) | ⬜ |
| M9 PortMaster package + FPS vs box64 | ⬜ |

### 🙋 Where help is needed

The recompiled CRT runs its startup, but during C++ static initialization a **return-address
sentinel leaks into `esp`/`ebp`** inside some CRT functions that perform raw stack surgery
incompatible with the recomp call/ret model. Native HLE of `__EH_prolog` and `_SEH_prolog4`
is done, but neither is the leak source.

- Current backtrace: `DllMain → sub_78542B94 → sub_78562B86 → _setmbcp → crash (invalid esp)`
- Wanted: the function that writes a stack value (the return address) back into `esp`/`ebp`.
- Debug tooling: the crash handler prints a **gdb-free backtrace** (the recomp uses real C
  calls, so the host backtrace *is* the guest call chain), the last lifted function entered,
  and the fault as a guest VA.

Ideas, PRs and discussion very welcome — see [docs/01-ARCHITECTURE.md](docs/01-ARCHITECTURE.md).

### Reproducing (you need your own game copy)

Requirements: Windows + WSL (gcc), Python 3.10 with `capstone pefile unicorn keystone-engine`.

```bash
# 1. pcrecomp, pinned + patched (patches from nfsu2-recompiled-r36s)
git clone https://github.com/sp00nznet/pcrecomp && cd pcrecomp
git checkout 35548b8 && git am ../nfsu2-recompiled-r36s/patches/pcrecomp/*.patch && cd ..

# 2. Put YOUR dontstarve_steam.exe in work/, and msvcr90.dll + msvcp90.dll
#    (x86, 9.0.30729.x — from your Windows WinSxS or the game's vcredist)

# 3. Lift (Windows Python) — see scripts/
python scripts/run_lift.py catalog work/dontstarve_steam.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift    work/dontstarve_steam.exe work/catalog.json src/recomp/gen
bash   scripts/lift_dll.sh work/msvcr90.dll msvcr90_
bash   scripts/lift_dll.sh work/msvcp90.dll msvcp90_
python scripts/find_data_funcs.py work/msvcr90.dll work/msvcr90_catalog.json config/exclude.txt

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

![fase](docs/badge-phase-pt.svg)

**Cobertura de lift por módulo** (cada célula ≈ 16 funções; cinza = dados podados do `.text`, nunca chamados):

![mapa de progresso](docs/progress-pt.svg)

![marcos](docs/milestones-pt.svg)

| Marco | Status |
|---|---|
| M0 Decisão de input + toolchain | ✅ feito |
| M1 Catálogo de funções — 17.033 funções | ✅ feito |
| M2 Lift para C99 — 4,2M linhas, **0 falhas** | ✅ feito |
| M3 Compila + linka (exe + 2 DLLs do CRT liftadas) | ✅ feito |
| M4 Host arena, bind da IAT (955/955), `DllMain` do CRT roda | ✅ feito |
| **M5 Inicialização do CRT** | 🟡 **em progresso — parede atual** |
| M6 Menu principal nativo | ⬜ |
| M7 Estabilidade | ⬜ |
| M8 Áudio (shim do FMOD Ex) | ⬜ |
| M9 Pacote PortMaster + FPS vs box64 | ⬜ |

### 🙋 Onde precisamos de ajuda

O CRT recompilado roda o startup, mas durante a inicialização estática C++ um **sentinela de
endereço de retorno vaza para `esp`/`ebp`** dentro de funções do CRT que fazem cirurgia crua na
pilha, incompatível com o modelo de call/ret do recomp. Já temos HLE nativo de `__EH_prolog` e
`_SEH_prolog4`, mas nenhum dos dois é a fonte do vazamento.

- Backtrace atual: `DllMain → sub_78542B94 → sub_78562B86 → _setmbcp → crash (esp inválido)`
- Procura-se: a função que escreve um valor de pilha (o endereço de retorno) de volta em `esp`/`ebp`.
- Ferramenta de debug: o crash handler imprime um **backtrace sem gdb** (o recomp usa calls C
  reais, então o backtrace do host *é* a cadeia de chamadas do guest), a última função liftada
  e o fault como VA do guest.

Ideias, PRs e discussão são muito bem-vindos — veja [docs/01-ARCHITECTURE.md](docs/01-ARCHITECTURE.md).

### Reproduzindo (precisa da sua cópia do jogo)

Requisitos: Windows + WSL (gcc), Python 3.10 com `capstone pefile unicorn keystone-engine`.
Os comandos são os mesmos da seção em inglês acima.

---

## Repository layout / Estrutura

```
docs/          design decisions, architecture, status page (index.html)
scripts/       lift + build drivers (run_lift.py, lift_dll.sh, find_data_funcs.py, build_*.sh, gen_status.py)
src/runtime/   arena host (ds_host.c), shim ABI (ds_hle.h), kernel32/CRT shims (ds_kernel32.c)
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
