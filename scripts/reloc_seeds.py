#!/usr/bin/env python3
"""Function seeds from base relocations: absolute pointers to function starts.

  reloc_seeds.py <image.dll> <seeds.json>   (merges into seeds.json, dedup by address)

A DLL's .reloc lists every absolute address in the image. Those that point into
an executable section include the function pointers: CRT init tables
(__xi_a..__xi_z, __xc_a..__xc_z), vtables, callback tables, `push offset fn`.
Recursive descent from the exports never reaches them, because nothing calls
them directly -- _initterm walks the table with `call [esi]`, and each missing
entry is an unresolved ICALL at runtime.

The VC9 CRT DLLs merge .rdata into .text, so where the pointer lives says
nothing. The target decides: it must look like a function start -- preceded by
int3/nop padding, or opening with the `mov edi, edi` hot-patch prologue. Jump
table case labels (mid-function, after a jmp or ret) fail both tests.
"""
import json, os, sys
import pefile

IMAGE_SCN_MEM_EXECUTE = 0x20000000


def reloc_seeds(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
             bool(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) for s in pe.sections]

    def sec_of(rva):
        for lo, hi, x in secs:
            if lo <= rva < hi:
                return (lo, hi, x)
        return None

    out = set()
    for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
        for e in blk.entries:
            if e.type != 3:                      # IMAGE_REL_BASED_HIGHLOW
                continue
            va = int.from_bytes(img[e.rva:e.rva + 4], 'little')
            t = sec_of(va - base)
            if not t or not t[2]:                # must point at code
                continue
            r = va - base
            prev = img[r - 1] if r > t[0] else 0xCC
            head = img[r:r + 2]
            if prev in (0xCC, 0x90) or head == b'\x8b\xff':
                out.add(va)
    return out


def main():
    dll, seeds_path = sys.argv[1], sys.argv[2]
    seeds = json.load(open(seeds_path)) if os.path.exists(seeds_path) else []
    have = {(e['address'] if isinstance(e, dict) else e) for e in seeds}
    new = sorted(reloc_seeds(dll) - have)
    seeds += [{'address': a} for a in new]
    json.dump(seeds, open(seeds_path, 'w'))
    print(f'  reloc seeds: +{len(new)} (total {len(seeds)})')


if __name__ == '__main__':
    main()
