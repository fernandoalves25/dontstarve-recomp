# Don't Starve (native recompilation) — PortMaster

This port runs Don't Starve natively on ARM64 handhelds (R36S and other
ArkOS / PortMaster devices). The game's x86 code was recompiled to C by
[dontstarve-recomp](https://github.com/fernandoalves25/dontstarve-recomp), so
there is no emulator and no box64. **You need your own copy of the game.**

## What to copy

Copy these files from your **DRM-free standalone** install (Klei's own build,
not the Steam one) into `ports/dontstarve-recomp/`:

| From your install | To |
|---|---|
| `bin/dontstarve.exe` | `gamedata/bin/dontstarve.exe` |
| `data/` (the whole folder) | `gamedata/data/` |
| `mods/` (if you have it) | `gamedata/mods/` |
| `msvcr90.dll`, `msvcp90.dll` (VC9 x86 runtime 9.0.30729.x, from a vcredist) | `crt/` |

The recompiled code was built from one exact `dontstarve.exe`; a different
build of the exe will not match it.

## First run

On the first run the port creates `home/` (saves, `settings.ini`, the game's
`log.txt`). If `python3` is on the device it also fills in the Vorbis setup
headers for the low-sample-rate sounds (`tools/audio/import_setups.py`,
needs an internet connection once). Without it, run that script on a PC with
`DS_GAME_DIR` and `DS_HOME_DIR` pointing at your copied `gamedata/` and
`home/` folders.

## Bigger HUD

A small mod in `gamedata/mods/r36s-hud` makes the in-game HUD (inventory,
status, crafting tabs, map) and the hover/speech text 1.4x larger. The
launcher enables it; the first time, the game shows a "Mods Installed" notice:
pick "I understand". Change the size in Mods → Bigger HUD → Configure
(1.0x–1.5x; 1.5x is the most a 640x480 screen fits), together with the game's
own Options → HUD Size. To turn it off: Mods menu, or `DS_BIG_HUD=0`.

## Controls

The device's buttons act as an Xbox pad, which Don't Starve supports out of
the box: the game uses its own controller layout and shows the buttons on
screen.

---

# Don't Starve (recompilação nativa) — PortMaster

Este port roda Don't Starve nativo em portáteis ARM64 (R36S e outros com
ArkOS / PortMaster). O código x86 do jogo foi recompilado para C pelo
[dontstarve-recomp](https://github.com/fernandoalves25/dontstarve-recomp):
sem emulador e sem box64. **Você precisa da sua própria cópia do jogo.**

## O que copiar

Copie estes arquivos da sua instalação **standalone DRM-free** (o build da
própria Klei, não o da Steam) para `ports/dontstarve-recomp/`:

| Da sua instalação | Para |
|---|---|
| `bin/dontstarve.exe` | `gamedata/bin/dontstarve.exe` |
| `data/` (a pasta inteira) | `gamedata/data/` |
| `mods/` (se tiver) | `gamedata/mods/` |
| `msvcr90.dll`, `msvcp90.dll` (runtime VC9 x86 9.0.30729.x, de um vcredist) | `crt/` |

O código recompilado foi gerado a partir de um `dontstarve.exe` exato; outro
build do exe não vai bater.

## Primeira execução

Na primeira execução o port cria `home/` (saves, `settings.ini`, o `log.txt`
do jogo). Se houver `python3` no aparelho, ele também completa os cabeçalhos
de setup Vorbis dos sons de taxa baixa (`tools/audio/import_setups.py`,
precisa de internet uma vez). Sem ele, rode esse script num PC com
`DS_GAME_DIR` e `DS_HOME_DIR` apontando para as pastas `gamedata/` e `home/`
copiadas.

## HUD maior

Um mod pequeno em `gamedata/mods/r36s-hud` deixa o HUD do jogo (inventário,
status, abas de criação, mapa) e os textos de hover/fala 1,4x maiores. O
lançador o habilita; na primeira vez o jogo mostra o aviso "Mods Installed":
escolha "I understand". Mude o tamanho em Mods → Bigger HUD → Configure
(1,0x–1,5x; 1,5x é o máximo que cabe numa tela 640x480), junto com o
Options → HUD Size do próprio jogo. Para desligar: menu Mods, ou `DS_BIG_HUD=0`.

## Controles

Os botões do aparelho funcionam como um controle de Xbox, que o Don't Starve
já suporta: o jogo usa o próprio layout de controle e mostra os botões na tela.
