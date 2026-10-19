# Performance / Desempenho

**EN** · [PT](#pt)

Goal: 60 fps on the R36S (Rockchip RK3326, 4× Cortex-A35 @ 1.3–1.5 GHz,
Mali-G31). The game itself caps at 60 fps and simulates at 30 Hz.

## How it is measured

`scripts/bench.sh` loads a saved world, stands still, skips the draws
(`DS_NO_DRAW=1`, so a software renderer does not hide the game's own cost)
and averages the main thread's **work per frame**: the frame time minus the
time the game's frame limiter spends waiting. Repeat runs agree within about
±0.25 ms. `DS_PROF=1` adds per-frame counts: draws, import calls and
indirect calls, plus the busiest imports and who calls them.

In a world the rendering is ~170 draws and ~17,000 vertices per frame,
which is light for the Mali GPU. The CPU side decides the frame rate.

## What was done (x86 Xeon @ 2.8 GHz, same scene)

| Change | Work/frame |
|---|---|
| Starting point: -O0 + coverage tracing (every earlier fps number) | — |
| -O2 build, frame limiter busy-waiting on `QueryPerformanceCounter` (~136,000 polls a frame) | ~12 ms CPU |
| x87 stack as a sliding window (`RECOMP_X87_WINDOW`): each push/pop had shifted 8 registers + shadows, ~62,000 56-byte memmoves a frame | |
| Call-target cache for indirect calls (C++ virtuals, Lua C functions, imports) | |
| Guest memory at a fixed address (patch 0018): no reload of the base after every guest store | |
| Frame limiter sleeps instead of spinning (also frees the core: battery, heat) | 6.3 ms |
| FPU-heavy functions keep ST(0..7) in locals (patch 0019, `RECOMP_X87_LOCAL`): the hottest engine function, a 4×4 matrix multiply, 2.2× faster | |
| Loop/inlining passes (`-funroll-loops -ftracer -finline-functions …`, see `scripts/build_arm64.sh`) | **4.5 ms** |

| Callee-saved register trust (patch 0020, `abi32.py`): a dataflow proves which functions keep ebx/esi/edi/ebp (11,613 of 16,146); after calls to them (93% of call sites) the caller skips reloading the four. Checked with `RECOMP_ABI_CHECK` builds over a new game and a loaded save: no violation | ~same on x86 |
| Flags across calls only where read: 2 of 106,297 call sites in the exe read flags a callee returns, so 26,143 returns stop publishing them | ~same on x86 |
| Native `_getptd_noexit` (the CRT's per-thread data, ~2,000 calls a frame, 4 imports each) | |

The last three remove instructions the Xeon's out-of-order core hid (the
reloads read what the callee had just stored); the in-order Cortex-A35 runs
every instruction, so they should count more there.

Where the time goes now: the C++ engine ~50%, the embedded Lua 5.1 VM ~20–30%
(`luaV_execute` 0x5B4A00, `luaH_get` 0x5B6200), the CRT and the host the
rest. The hottest instructions of `luaH_get` wait on memory (walking Lua's
hash chains), so the remaining cost is largely the game's own memory access,
which native code would pay too.

## What that means on the R36S

A Cortex-A35 is roughly 8–10× slower than this Xeon core single-threaded,
so 4.5 ms here is very roughly **35–45 ms per frame there (~22–28 fps)**,
before the Mali driver's own CPU work. These figures are estimates until
someone runs it on the device. Ways forward, largest first:

1. ~~The lifted calling convention~~: done (callee-saved trust, flags).
2. **A native Lua VM.** Run `luaV_execute` and the table functions as native
   C on the guest's own Lua state layout (32-bit pointers in guest memory).
3. ~~Fewer CRT round trips~~: `_getptd` is native now.
4. **A 30 fps cap** as a fallback: the r36s-hud mod's "Frame rate cap"
   option calls the engine's own `TheSim:SetTargetFrameRate(30)`.

<a id="pt"></a>
## PT

Meta: 60 fps no R36S (RK3326, 4× Cortex-A35 a 1,3–1,5 GHz, Mali-G31). O
próprio jogo limita a 60 fps e simula a 30 Hz.

**Como se mede:** o `scripts/bench.sh` carrega um mundo salvo, fica parado,
pula os draws (`DS_NO_DRAW=1`, para o renderizador por software não esconder
o custo do jogo) e tira a média do **trabalho por frame** da thread principal:
o tempo do frame menos o tempo que o limitador de frames do jogo passa
esperando. Execuções repetidas batem em cerca de ±0,25 ms. Com `DS_PROF=1`
saem contagens por frame: draws, chamadas de import e chamadas indiretas,
além dos imports mais chamados e de quem os chama. Num mundo a renderização
são ~170 draws e ~17.000 vértices por frame, pouco para a GPU Mali: é a CPU
que decide o frame rate.

**O que foi feito** (x86 Xeon a 2,8 GHz, mesma cena):
- a pilha x87 virou uma janela deslizante: cada push/pop deslocava 8
  registradores, ~62.000 memmoves de 56 bytes por frame;
- cache de alvos para as chamadas indiretas;
- memória do guest num endereço fixo (patch 0018);
- o limitador de frames dorme em vez de girar (~136.000 consultas ao relógio
  por frame), o que também libera o núcleo (bateria, calor);
- funções pesadas em FPU mantêm ST(0..7) em locais (patch 0019): a
  multiplicação de matriz 4×4 ficou 2,2× mais rápida;
- passes de laço/inlining no compilador.

Resultado: **~12 → 4,5 ms de trabalho por frame**.

**No R36S:** um Cortex-A35 é algo como 8–10× mais lento que esse Xeon numa
thread, então 4,5 ms aqui são grosso modo **35–45 ms lá (~22–28 fps)**, antes
do custo de CPU do driver Mali. São estimativas até alguém rodar no
aparelho. Próximos passos, do maior para o menor:
1. ~~a convenção de chamada do código liftado~~: feito (registradores
   preservados e flags só onde são lidas, patch 0020, conferido com builds
   `RECOMP_ABI_CHECK`);
2. uma VM Lua nativa;
3. ~~menos idas e voltas ao CRT~~: o `_getptd` agora é nativo;
4. como plano B, um limite de 30 fps (opção "Frame rate cap" do mod
   r36s-hud).
