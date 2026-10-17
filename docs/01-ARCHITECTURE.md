# Arquitetura do port nativo (Don't Starve → R36S ARM64)

Recompilação estática de `dontstarve_steam.exe` (x86-32 PE) para C99 nativo via
pcrecomp (`a847ef6` = `35548b8` + 14 patches NFSU2), compilado para aarch64
(RK3326, Cortex-A35, glibc ≤ 2.30). Sem box64, sem tradução em runtime.

Modelo copiado do `nfsu2-ref` (mesmo aparelho, mesma ferramenta). O que muda é
descrito em "Deltas vs NFSU2" abaixo.

```
   dontstarve_steam.exe (x86-32)
            │  pcrecomp: pe_analyze → disasm32 (+ seeds vtable/rtti) → lift32 (+simd32)
            ▼
   ~N funções C99  (src/recomp/gen/recomp_*.c)   [N a medir no catálogo]
            │
            ├── host debug (opcional, Windows/MSVC x86) — bring-up e difftest
            └── R36S aarch64 ───────────────────────────────────────────────┐
                 runtime: pcrecomp/runtime/recomp32 + win32hle               │
                 + shims DS (abaixo) → link SDL2 / GLESv2 / EGL / pthread    │
                 → pacote PortMaster em /roms/ports/                          │
```

## Baseline do binário (medido 2026-10-17)

- **Imports:** 666 funções de 22 DLLs.
- **C++:** RTTI praticamente **desligado** (só 1 classe com COL). Fonte real de
  seeds de método virtual = `vtable_scan` estrutural: **345 vtables, 1.899
  métodos virtuais** distintos. Esses seeds alimentam `disasm32 --seed-functions`.
- **Contagem total de funções:** a medir no passo de catálogo (disasm32). NFSU2
  (código 2,6 MB+) deu 27.742; o DS tem ~2,6 MB de código → mesma ordem de
  grandeza; número real define o denominador do "% liftado".

## Mapa de imports → estratégia de shim

| Grupo | DLLs (nº funções) | Estratégia |
|---|---|---|
| **CRT / C++ runtime** | MSVCR90 (182), MSVCP90 (113) | `win32hle`/`recomp32` cobrem a superfície comum; lacunas viram stub e são preenchidas iterativamente. Maior superfície única (295). |
| **Win32 core** | KERNEL32 (75), USER32 (44), GDI32 (9), ADVAPI32 (3), SHELL32 (5), ole32 (6), OLEAUT32 (3), WINMM (2) | `win32hle` reaproveitado do pcrecomp/NFSU2. Janela/superfície criada aqui (ver gráficos). |
| **Gráficos** ⭐ | libGLESv2 (76), libEGL (9) | **Ponte fina GLES2/EGL → nativo (Mali/gl4es).** Passa quase direto; shaders GLSL ES compilam direto. **Sem MojoShader, sem D3D9.** |
| **Áudio** ⚠ | fmodex (21), fmod_event (24) | **Peça custom mais pesada do DS.** FMOD Ex 4.44 não tem build ARM64. Implementar o subconjunto da API FMOD Ex + Event que o DS usa sobre backend nativo (SDL_audio/miniaudio), decodificando bancos FSB5/Vorbis. Permitir bring-up **mudo** primeiro. |
| **Input** | DINPUT8 (1), XINPUT1_3 (5) | Ponte para SDL2 GameController / evdev. Pequeno. |
| **Rede (single-player)** | WSOCK32 (29), WLDAP32 (16), WS2_32 (2) | Stub para single-player (offline). |
| **Plataforma/telemetria** | steam_api (12), twitchsdk_32 (28) | Stub. steam_api → "não rodando sob Steam" (fallback offline, DRM-free, cópia do próprio usuário; **nunca forjar licença**). Twitch irrelevante no handheld. |

## Deltas vs NFSU2 (o que muda neste port)

| Aspecto | NFSU2 | Don't Starve |
|---|---|---|
| **Gráficos** | D3D9 → precisa `d3d9_gles` + **MojoShader** (maior componente custom) | **Já é GLES2 (ANGLE)** → ponte GLES2 nativa, **MojoShader descartado** ✅ |
| **Janela/input no EXE** | — | EXE Windows **não importa SDL2**; faz janela via Win32 + contexto via ANGLE EGL. Logo a janela/surface real vem do `win32hle` (SDL2/KMSDRM por baixo), não do jogo. |
| **Áudio** | DirectSound/AAudio | **FMOD Ex 4.44 + FMOD Event** → shim de API novo (peça custom mais pesada) ⚠ |
| **Linguagem de script** | — | **Lua 5.1 interpretado** embutido. Sem código auto-modificável → lifta como C++ normal. É o gargalo do box64 (dispatch da VM) que o nativo elimina. |
| **C++/RTTI** | — | RTTI off → seeds vêm de `vtable_scan` (1.899 métodos). Loop iterativo ICALL forte (ver riscos). |
| **DLCs/mods/PT-BR** | — | Preservados: recompila só o EXE; gamedata (DLCs RoG+SW+Hamlet, mods, PT-BR workshop-535878841, UI de PC) é enviado como-está, igual ao port box64. |

## Toolchain

- **Lift (Windows, Python 3.10):** capstone 5.0.7, pefile 2023.2.7, unicorn
  2.1.4, keystone — **prontos**. Rodam agora.
- **Cross-build aarch64:** WSL tem `aarch64-linux-gnu-gcc 13.3.0`, mas é Ubuntu
  24.04 (**glibc ~2.39**). O aparelho é **glibc 2.30** → binário não carregaria
  (erro de versão de símbolo). ⚠ **A resolver:** buildar em container/distro
  antiga (glibc ≤ 2.30) com toolchain aarch64 — exatamente o que o NFSU2 fez
  (`nfsu2-ref/docker/` + `build_vm.sh`). Estudar na fase de build.

## Riscos a orçar (reais)

1. **Loop iterativo ICALL/ITAIL.** Binário retail C++ sem símbolos, RTTI off →
   ciclo rodar→crashar→achar alvo indireto não resolvido→re-liftar.
   `RECOMP_STRICT_ICALL` (patch 0003) + `disasm/seed_from_log.py` são o mecanismo.
2. **Exceções C++ / STL** (113 símbolos MSVCP90). Caminho 32-bit: `recomp32` +
   `fexceptions`.
3. **FMOD Ex sem ARM64** → shim de API (item de áudio acima).
4. **Mismatch de glibc no cross-build** (toolchain acima).
5. **x87/MMX** podem virar `RECOMP_TODO` no lift32 (patch 0002 cobre MMX/SSE/SSE2;
   x87 tem suporte de ambiente via patches 0004/0008/0014).

## Roadmap / marcos (honesto — projeto de meses)

- [x] **M0 — Decisão + toolchain.** Input decidido (32-bit PE), pcrecomp patchado
  (`a847ef6`), binário analisado, deps de lift instaladas. ← **aqui**
- [ ] **M1 — Catálogo.** `disasm32` + seeds vtable/rtti → contagem total de
  funções (denominador do %).
- [ ] **M2 — Geração C.** `lift32` → `src/recomp/gen/recomp_*.c`, 0 falhas de lift.
- [ ] **M3 — Compila/linka** (host debug e/ou aarch64).
- [ ] **M4 — CRT/entry/WinMain** rodam recompilados.
- [ ] **M5 — Janela + contexto GLES2 + primeiro frame.**
- [ ] **M6 — Menu nativo** (marco "abre no menu"). Áudio pode estar mudo.
- [ ] **M7 — Estabilidade** (loop ICALL fechado o bastante p/ jogar).
- [ ] **M8 — Áudio** (shim FMOD).
- [ ] **M9 — Empacotar PortMaster** ao lado do port box64 e **medir FPS vs box64**.

Métrica de progresso reportada: **% de funções liftadas** + marco funcional atual.
