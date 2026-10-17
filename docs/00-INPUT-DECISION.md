# Decisão de input: qual binário recompilar

**Data:** 2026-10-17
**Decisão:** recompilar o **EXE Windows x86-32** (`dontstarve_steam.exe`), pelo
caminho **32-bit PE** do pcrecomp. **NÃO** o ELF Linux x86-64.

Isto **corrige** a recomendação inicial (que sugeria o ELF Linux x86-64 por ser
o "caminho 64-bit mais maduro"). A premissa não sobrevive ao contato com a
ferramenta.

---

## Por que o ELF Linux foi descartado

O pcrecomp **não tem front-end de ELF**. Evidência direta no repo
(`pcrecomp/`, HEAD `35548b8` + 14 patches = `a847ef6`):

1. **A matriz de formatos do README tem 4 colunas e nenhuma é ELF:**
   `16-bit DOS (MZ)` · `16-bit Windows (NE)` · `32-bit PE` · `64-bit PE`.
   Os únicos loaders de binário em `tools/` são `pe/`, `ne/` e `macho/`.
   Não existe `tools/elf/`.

2. **O "caminho 64-bit" é Win64 PE, não Linux.** A seção "What the 64-bit path
   does" descreve explicitamente estruturas **exclusivas de PE/Windows**:
   - lê `.pdata`/`.xdata` (dados de exceção SEH do Windows x64);
   - varre relocations `DIR64` (tipo de reloc do PE);
   - preenche a **IAT** do PE para forward de imports;
   - trata exceções C++ via `__CxxFrameHandler3` / `_CxxThrowException`
     (ABI da MSVC C++ runtime), em `eh64.c`.
   Um ELF Linux usa `.eh_frame`/DWARF, ABI Itanium C++, GOT/PLT e tipos de
   reloc de ELF — **nada** disso. Usar o ELF exigiria escrever um loader ELF
   inteiro + readaptar o lifter 64-bit de PE→ELF. Isso é construir um backend
   novo, não "usar o pcrecomp".

3. **Não há build Windows 64-bit do Don't Starve.** Logo o "caminho 64-bit"
   (Win64 PE) também não se aplica — o único PE que existe é 32-bit.

Conclusão: a confusão original foi equiparar "64-bit" a "ELF Linux x86-64"
porque a build Linux é 64-bit. No pcrecomp, "64-bit" = **Win64 PE**.

---

## Por que o 32-bit PE é o caminho certo

- É o caminho **mais exercitado** do pcrecomp (a maioria dos projetos da lista
  são 32-bit PE ou 16-bit).
- **Existe precedente idêntico no mesmo aparelho:** o NFSU2 recompilado para
  R36S (`manuelpllull/nfsu2-recompiled-r36s`) recompila `SPEED2.EXE` (x86-32
  PE) → 27.742 funções C, 0 falhas de lift, rodando nativo no RK3326. Ele
  fornece um scaffold de build + pacote PortMaster + 14 patches do pcrecomp
  prontos — tudo reaproveitável.
- Os 14 patches do NFSU2 mexem **só no caminho 32-bit** (`lift32`, `native32`,
  `recomp_types`) — nenhum toca `lift64`. Confirma que o precedente é 32-bit PE.

## Custo que o 32-bit PE traz (e que o ELF teoricamente evitaria)

O alvo passa a ser Windows, então precisamos da superfície de emulação Win32
(`win32hle`: kernel32/user32/gdi32 reimplementados sobre SDL2/POSIX) e do CRT
VC9 (MSVCR90/MSVCP90). **Mas isso já está construído** no pcrecomp + NFSU2 e é
testado neste exato aparelho. É um custo conhecido, não um bloqueio.

## Binário alvo (confirmado por `pe_analyze`)

| Propriedade | Valor |
|---|---|
| Arquivo | `dontstarve_steam.exe` (cópia do próprio usuário, Steam) |
| Tamanho | 3.361.280 bytes |
| SHA-256 | `13b7c9c19d9ab03e4ada4ea6217b7bdac851c30f68b90f09ae5b379cdd42d64f` |
| Formato | PE32, i386, Windows GUI |
| Linker | 9.00 (VC9 / Visual Studio 2008) |
| ImageBase / Entry | `0x00400000` / `0x00662ABD` |
| Código | `0x00401000`–`0x0067DC00` = 2.608.128 bytes |
| Seções | `.text .rdata .data .tls .rsrc .reloc` (limpo, **não packed**) |
| `.reloc` | presente (134 KB) → `normalize_exe` provavelmente dispensável |

Ver superfície de imports e plano de shims em [01-ARCHITECTURE.md](01-ARCHITECTURE.md).
