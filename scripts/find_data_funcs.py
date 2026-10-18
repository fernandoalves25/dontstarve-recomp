#!/usr/bin/env python3
"""Flag catalog 'functions' that are really DATA decoded as code, so the lift
excludes them (stubs). A function is data if its body contains an opcode that
never appears in real 32-bit user-mode VC9 output. Exports are ground-truth
code and are NEVER flagged. Appends '<hexVA>  data-as-code' lines to an exclude
file. Usage: find_data_funcs.py <dll> <catalog.json> <exclude.txt>
"""
import sys, json, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# Opcodes impossible in real 32-bit user-mode compiler/CRT code. (Deliberately
# excludes fisttp/fcomi/SIMD/lock/rep/ud2 -- those ARE real, just unlifted.)
JUNK = {
    'aaa', 'aas', 'daa', 'das', 'arpl', 'bound',
    'insb', 'insd', 'insw', 'outsb', 'outsw', 'outsd',
    'iret', 'iretd', 'sti', 'cli', 'hlt', 'into', 'salc',
    'popaw', 'pushaw', 'ljmp', 'lcall', 'les', 'lds',
    'sldt', 'lgdt', 'lidt', 'lldt', 'ltr', 'lmsw', 'clts', 'rsm', 'str',
    'in', 'out',
}

dll, catalog_path, exclude_path = sys.argv[1], sys.argv[2], sys.argv[3]
pe = pefile.PE(dll, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
exports = set()
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if e.address:
            exports.add(base + e.address)

# whole-image bytes addressed by VA
img = pe.get_memory_mapped_image()
def rd(va, n):
    off = va - base
    return img[off:off+n] if 0 <= off < len(img) else b''

md = Cs(CS_ARCH_X86, CS_MODE_32)
cat = json.load(open(catalog_path))
flagged = []
kept_export_junk = 0
for f in cat['functions']:
    a, end = f['address'], f['end']
    if end <= a or end - a > 0x20000:
        continue
    data = rd(a, end - a)
    bad = False
    for insn in md.disasm(data, a):
        if insn.mnemonic in JUNK:
            bad = True
            break
    if bad:
        if a in exports:
            kept_export_junk += 1   # never prune a real export
        else:
            flagged.append(a)

with open(exclude_path, 'a') as fh:
    for a in sorted(flagged):
        fh.write(f"{a:08x}  data-as-code\n")
print(f"{dll}: flagged {len(flagged)} data-as-code funcs (of {len(cat['functions'])}); "
      f"kept {kept_export_junk} export(s) that tripped the filter")
