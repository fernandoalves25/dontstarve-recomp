# Don't Starve — recompilação nativa ARM64 para R36S

Terceira via de port do Don't Starve (single-player, versão PC completa) para o
R36S / clone K36 (RK3326, ArkOS): **recompilação estática** do EXE Windows com
[pcrecomp](https://github.com/sp00nznet/pcrecomp), gerando C99 nativo compilado
para aarch64 — **sem box64, sem emulação em runtime**.

Objetivo: superar o teto de ~20-22 FPS do port box64 (CPU-bound no overhead de
tradução x86→ARM64 sobre a VM Lua) preservando **tudo** da versão PC: DLCs
(RoG + Shipwrecked + Hamlet), mods, tradução PT-BR (workshop-535878841) e UI de PC.

## Status

**M0 concluído** — decisão de input + toolchain. Ver [docs/01-ARCHITECTURE.md](docs/01-ARCHITECTURE.md)
para o roadmap completo. Próximo: M1 (catálogo de funções).

## Estrutura

```
recompiled/
├── docs/              decisões e arquitetura (00-INPUT-DECISION, 01-ARCHITECTURE)
├── scripts/           drivers do pipeline de lift
├── src/runtime/       shims específicos do DS (GLES2/EGL, FMOD, input, overrides)
├── ports/r36s/        build cross-aarch64 + pacote PortMaster
├── work/              material derivado do jogo (gitignored: exe, análises, seeds)
├── pcrecomp/          ferramenta (clone @a847ef6 = 35548b8 + 14 patches; gitignored)
├── nfsu2-ref/         projeto de referência no mesmo aparelho (gitignored)
└── build/             saída de build (gitignored)
```

> `pcrecomp/` e `nfsu2-ref/` são clones externos, não versionados aqui. Para
> reproduzir: clonar pcrecomp em `35548b8` e aplicar
> `nfsu2-ref/patches/pcrecomp/*.patch` com `git am`.

## Legal

Opera **apenas** sobre a cópia do próprio dono do jogo. Nenhum binário ou asset
do jogo é versionado ou publicado (ver `.gitignore` e pcrecomp `docs/PUBLISHING.md`).
Nunca forja nem emula licença.
