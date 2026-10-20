# Don't Starve on the R36S via box64 (alternative path)

**English** · [Português](#português)

> **Which one do I want?** This repository has two separate ways to run Don't Starve on the
> R36S. They share nothing at run time; pick one.
>
> | | **box64 port** (this folder) | **Native recompilation** (rest of the repo) |
> |---|---|---|
> | How | the official **Linux x86-64** build, emulated by box64 | the **Windows** exe, recompiled to native ARM64 C |
> | Game files | GOG Linux installers | the DRM-free Windows standalone build |
> | Status | **playable today**, ~20–25 fps in a world | experimental, ~12 fps in a world |
> | Use it if | you want to play now | you want to help with the recompilation |

## What it is

A PortMaster port: `Dont Starve.sh` plus the `dontstarve/` folder go in `ports/` on the SD card.
It runs the official Linux build of Don't Starve (Reign of Giants, Shipwrecked and Hamlet if
you own them) through **box64**, with OpenGL translated to OpenGL ES by **gl4es**, the engine's
low-spec modes on and textures halved losslessly.

**Measured on an R36S** (ArkOS, CPU 1368 MHz): main menu at 30 fps, **20–25 fps in a world**,
playable with the controller only. The game is CPU-bound: its simulation (Lua) runs on one
thread at 100% of a core. Memory (1 GB) is the other limit.

### Needs the GOG version

The **Steam build does not work**: it quits unless the Steam client runs, and there is no Steam
client for the R36S (getting past that would be breaking DRM, which this port does not do). Use
the **GOG Linux installers** (`.sh`), base game plus any DLC.

## What is in this folder (and what is not)

In git: the launcher, `port.cfg`, the engine settings, the controller map, the
`r36s_performance` mod, the `r36s_hooks` source, `ds_optimize.py` (installer extraction and
lossless texture halving), the ES release helper, a box64 build script and the licenses.

**Not in git** — put them in `dontstarve/` yourself:

| Path | What | Where to get it |
|---|---|---|
| `box64/box64` | box64 for aarch64 (tested: v0.3.7) | build it with `dontstarve/tools/build_box64.sh` (RK3326 target, old glibc), or take it from a PortMaster box64 port |
| `gl4es.aarch64/libGL.so.1`, `libEGL.so.1` | gl4es | [ptitSeb/gl4es](https://github.com/ptitSeb/gl4es) or a PortMaster port that ships it |
| `libs.x64/libgcc_s.so.1`, `libstdc++.so.6` | x86-64 GCC runtime the game links | Debian/Ubuntu `amd64` packages `libgcc-s1` and `libstdc++6` (`dpkg-deb -x`) |
| `libs.x64/r36s_hooks.so` | the FMOD and cursor fixes | `gcc -O2 -shared -fPIC -fno-stack-protector -U_FORTIFY_SOURCE -o r36s_hooks.so tools/r36s_hooks/r36s_hooks.c` on any x86-64 Linux |
| `gamedata/` | **your** game | the GOG installers (see Install) |
| `mods/r36s-hud/` | optional bigger HUD/text mod | copy `../port/mods/r36s-hud` from this repository |

## Install

1. Copy `Dont Starve.sh` and `dontstarve/` (with the files above) to `ports/` on the card.
2. Put the GOG `.sh` installers in `ports/dontstarve/gamedata/` — or, faster, on a PC:
   `python dontstarve/tools/ds_optimize.py extract gog_dont_starve*.sh --out dontstarve/gamedata`
   then `python dontstarve/tools/ds_optimize.py optimize dontstarve/gamedata`.
3. Open **Dont Starve** in the Ports menu. The first start extracts and optimizes (minutes).

## Sound fixes (choppy audio)

The game's FMOD mixer runs emulated too. In a world it sat at **100% of a core** and the sound
stuttered no matter the buffer size; with the settings below it drops to ~65% and stays
smooth. All of them are in `port.cfg` and applied by `r36s_hooks` before FMOD starts:

| Setting | Value | Effect |
|---|---|---|
| `FMOD_BLOCKS` | `16` | mixes ~341 ms ahead (the game uses 4 blocks, 85 ms) |
| `AUDIO_BUFFER` | `16384` | SDL audio buffer in samples at 48 kHz (~340 ms) |
| `FMOD_VOICES` | `24` | sounds mixed at once (game: 64) |
| `FMOD_RESAMPLER` | `1` | no interpolation in the mixer (cheapest; 0 = the game's) |
| `FMOD_NOREVERB` | `1` | ignores the game's area reverb presets, the most expensive DSP |

Below 48 kHz (`FMOD_RATE`) the sound distorts, so the rate stays at the game's.

## Speed settings

| Setting | Default | What it does |
|---|---|---|
| `PERF_GOV` | `1` | CPU, GPU **and memory** governors to `performance` while playing, restored on exit. ArkOS boots with the memory at 328 MHz, which alone takes the game from ~20 to ~12 fps. |
| `HIDE_CURSOR` | `1` | hides SDL's mouse pointer (it was redrawn every frame: 18 → 22 fps) |
| `BOX64_FAST` | `1` | `SAFEFLAGS=0 FORWARD=1024 CALLRET=1` (no other box64 flag measured faster) |
| `SMALL_TEXTURES`, `OPTIMIZE_TEXTURES`, `TEX_16BIT` | `1` | the memory savers that let a world load in 1 GB |
| `PERF_MOD` | `1` | the `r36s_performance` mod: 30 Hz engine, no bloom/distortion/vignette, fewer rain/snow particles, controller fixes |
| `HUD_MOD` | `1` | enables `r36s-hud` (HUD 1.4x, all text 1.25x) when `mods/r36s-hud` exists |
| `PIN_MAIN` | `0` | main thread alone on one core: measured no gain, off |

Tried and not faster: box64 v0.4.4 (same 17–25 fps as v0.3.7), `BIGBLOCK`, `DIRTY`,
`WEAKBARRIER`, a 30 fps cap (frames that take ~50 ms then take two 33 ms slots: 15 fps).
What does help in game: smaller worlds and leaner bases (the cost grows with the number of
things near the player).

## Tips

- **DLC:** the game only offers the DLCs whose folders exist in `gamedata/data/` (`DLC0001` =
  Reign of Giants, `DLC0002` = Shipwrecked, `DLC0003` = Hamlet). Moving a folder out (not
  deleting) hides that DLC; pick Reign of Giants on the New Game screen.
- **Mods in the in-game menu are only saved with the Apply button.** Leaving the Mods screen
  with B cancels. Mods the launcher force-enables (`modsettings.lua`) never need the menu.
- Saves: `ports/dontstarve/conf/.klei/DoNotStarve/`.

---

## Português

> **Qual eu quero?** Este repositório tem dois jeitos separados de rodar Don't Starve no R36S.
> Eles não dependem um do outro; escolha um.
>
> | | **Port box64** (esta pasta) | **Recompilação nativa** (resto do repositório) |
> |---|---|---|
> | Como | o build oficial **Linux x86-64**, emulado pelo box64 | o exe **Windows**, recompilado para C nativo ARM64 |
> | Arquivos do jogo | instaladores Linux da GOG | o build standalone DRM-free de Windows |
> | Status | **jogável hoje**, ~20–25 fps no mundo | experimental, ~12 fps no mundo |
> | Use se | quer jogar agora | quer ajudar na recompilação |

## O que é

Um port PortMaster: `Dont Starve.sh` e a pasta `dontstarve/` vão em `ports/` no cartão. Ele
roda o build Linux oficial de Don't Starve (com Reino dos Gigantes, Shipwrecked e Hamlet, se
você tiver) pelo **box64**, com o OpenGL traduzido para OpenGL ES pelo **gl4es**, os modos de
baixo consumo do motor ligados e as texturas reduzidas pela metade sem perda.

**Medido num R36S** (ArkOS, CPU a 1368 MHz): menu a 30 fps, **20–25 fps no mundo**, jogável só
com o controle. O jogo é limitado pela CPU: a simulação (Lua) roda numa thread só, a 100% de um
núcleo. A memória (1 GB) é o outro limite.

### Precisa da versão GOG

O **build da Steam não funciona**: ele fecha se o cliente da Steam não estiver rodando, e não
existe cliente Steam para o R36S (contornar isso seria quebrar DRM, o que este port não faz).
Use os **instaladores Linux da GOG** (`.sh`): jogo base e as DLCs que quiser.

## O que está nesta pasta (e o que não está)

No git: o lançador, o `port.cfg`, as configurações do motor, o mapa do controle, o mod
`r36s_performance`, o código do `r36s_hooks`, o `ds_optimize.py` (extrai os instaladores e reduz
as texturas sem perda), o ajudante do EmulationStation, um script de build do box64 e as licenças.

**Fora do git** — coloque em `dontstarve/` você mesmo:

| Caminho | O que é | Onde conseguir |
|---|---|---|
| `box64/box64` | box64 para aarch64 (testado: v0.3.7) | compile com `dontstarve/tools/build_box64.sh` (alvo RK3326, glibc antiga) ou pegue de um port box64 do PortMaster |
| `gl4es.aarch64/libGL.so.1`, `libEGL.so.1` | gl4es | [ptitSeb/gl4es](https://github.com/ptitSeb/gl4es) ou um port do PortMaster que traga ele |
| `libs.x64/libgcc_s.so.1`, `libstdc++.so.6` | runtime GCC x86-64 que o jogo usa | pacotes `amd64` do Debian/Ubuntu `libgcc-s1` e `libstdc++6` (`dpkg-deb -x`) |
| `libs.x64/r36s_hooks.so` | os ajustes de FMOD e do cursor | `gcc -O2 -shared -fPIC -fno-stack-protector -U_FORTIFY_SOURCE -o r36s_hooks.so tools/r36s_hooks/r36s_hooks.c` em qualquer Linux x86-64 |
| `gamedata/` | **o seu** jogo | os instaladores da GOG (veja Instalação) |
| `mods/r36s-hud/` | mod opcional de HUD e textos maiores | copie `../port/mods/r36s-hud` deste repositório |

## Instalação

1. Copie `Dont Starve.sh` e `dontstarve/` (com os arquivos acima) para `ports/` no cartão.
2. Coloque os instaladores `.sh` da GOG em `ports/dontstarve/gamedata/` — ou, mais rápido, no PC:
   `python dontstarve/tools/ds_optimize.py extract gog_dont_starve*.sh --out dontstarve/gamedata`
   e depois `python dontstarve/tools/ds_optimize.py optimize dontstarve/gamedata`.
3. Abra **Dont Starve** no menu Ports. A primeira abertura extrai e otimiza (alguns minutos).

## Correções do som (áudio picotado)

O mixer do FMOD do jogo também roda emulado. No mundo ele ficava em **100% de um núcleo** e o
som picotava com qualquer tamanho de buffer; com os ajustes abaixo ele cai para ~65% e o som
fica contínuo. Todos estão no `port.cfg` e o `r36s_hooks` aplica antes de o FMOD iniciar:

| Opção | Valor | Efeito |
|---|---|---|
| `FMOD_BLOCKS` | `16` | mixa ~341 ms adiantado (o jogo usa 4 blocos, 85 ms) |
| `AUDIO_BUFFER` | `16384` | buffer de áudio da SDL, em amostras a 48 kHz (~340 ms) |
| `FMOD_VOICES` | `24` | sons mixados ao mesmo tempo (o jogo usa 64) |
| `FMOD_RESAMPLER` | `1` | sem interpolação no mixer (o mais leve; 0 = a do jogo) |
| `FMOD_NOREVERB` | `1` | ignora a reverb de cada área, o efeito mais caro |

Abaixo de 48 kHz (`FMOD_RATE`) o som distorce, então a taxa fica a do jogo.

## Opções de desempenho

| Opção | Padrão | O que faz |
|---|---|---|
| `PERF_GOV` | `1` | CPU, GPU **e memória** em `performance` enquanto joga, e de volta ao sair. O ArkOS liga com a memória a 328 MHz, o que sozinho derruba o jogo de ~20 para ~12 fps. |
| `HIDE_CURSOR` | `1` | esconde a seta do mouse da SDL (era redesenhada todo quadro: 18 → 22 fps) |
| `BOX64_FAST` | `1` | `SAFEFLAGS=0 FORWARD=1024 CALLRET=1` (nenhuma outra flag do box64 ganhou) |
| `SMALL_TEXTURES`, `OPTIMIZE_TEXTURES`, `TEX_16BIT` | `1` | as economias de memória que deixam um mundo carregar em 1 GB |
| `PERF_MOD` | `1` | o mod `r36s_performance`: motor a 30 Hz, sem bloom/distorção/vinheta, menos partículas de chuva/neve, correções do controle |
| `HUD_MOD` | `1` | liga o `r36s-hud` (HUD 1,4x, todos os textos 1,25x) quando existe `mods/r36s-hud` |
| `PIN_MAIN` | `0` | thread principal sozinha num núcleo: medido sem ganho, desligado |

Testado e não ficou mais rápido: box64 v0.4.4 (os mesmos 17–25 fps da v0.3.7), `BIGBLOCK`,
`DIRTY`, `WEAKBARRIER` e um limite de 30 fps (quadros de ~50 ms passam a ocupar duas fatias de
33 ms: 15 fps). O que ajuda dentro do jogo: mundos menores e bases enxutas (o custo cresce com o
número de coisas perto do personagem).

## Dicas

- **DLCs:** o jogo só oferece as DLCs cujas pastas existem em `gamedata/data/` (`DLC0001` = Reino
  dos Gigantes, `DLC0002` = Shipwrecked, `DLC0003` = Hamlet). Mover uma pasta para fora (sem
  apagar) esconde essa DLC; escolha Reino dos Gigantes na tela de Novo Jogo.
- **Os mods do menu do jogo só são salvos pelo botão Apply.** Sair da tela de Mods pelo B cancela.
  Os mods que o lançador força (`modsettings.lua`) nunca precisam do menu.
- Saves: `ports/dontstarve/conf/.klei/DoNotStarve/`.
