#!/bin/bash
# Lift a VC9 runtime DLL into prefixed recomp C, seeded from its export table.
# Runs the Windows lift pipeline (capstone/pefile). Usage: lift_dll.sh <dll> <prefix>
set -u
cd "$(dirname "$0")/.."
export PCRECOMP="$PWD/pcrecomp"
dll="$1"; prefix="$2"
name="$(basename "$dll" .dll)"
echo "=== $name: export seeds ==="
python -c "
import sys, json, pefile
pe = pefile.PE(r'$dll', fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
seeds = []
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if e.address:
            seeds.append({'address': base + e.address})
json.dump(seeds, open(r'work/${name}_seeds.json','w'))
print('  export seeds:', len(seeds))
"
# code pointers in data sections (CRT init tables, vtables): see reloc_seeds.py
python scripts/reloc_seeds.py "$dll" "work/${name}_seeds.json"
echo "=== $name: catalog ==="
python scripts/run_lift.py catalog "$dll" "work/${name}_seeds.json" "work/${name}_catalog.json"
echo "=== $name: lift (prefix $prefix) ==="
python scripts/run_lift.py lift "$dll" "work/${name}_catalog.json" src/recomp/gen --prefix "$prefix"
echo "=== $name: done ==="
