#!/usr/bin/env python3
"""Fold a runtime coverage run into docs/status.json.

Input  (local, gitignored): build/executed_vas.txt  - one hex VA per executed lifted function
                            work/*catalog.json      - the function catalogs
Output (public):            docs/status.json        - per module: executed count + a per-cell
                                                      bitmap ("1" = some function in that cell ran)
Only counts and a coarse bitmap are published - no addresses, no code.
Usage: python scripts/coverage_counts.py   then   python scripts/gen_status.py
"""
import json, os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
STATUS = os.path.join(ROOT, "docs", "status.json")
CATALOGS = {
    "dontstarve_steam.exe": "work/catalog.json",
    "msvcr90.dll": "work/msvcr90_catalog.json",
    "msvcp90.dll": "work/msvcp90_catalog.json",
}

executed = set()
with open(os.path.join(ROOT, "build", "executed_vas.txt")) as f:
    for line in f:
        line = line.strip()
        if line:
            executed.add(int(line, 16))

status = json.load(open(STATUS, encoding="utf-8"))
per = status["per_cell"]
total_exec = 0
for m in status["modules"]:
    cat = json.load(open(os.path.join(ROOT, CATALOGS[m["name"]])))
    addrs = sorted(f["address"] for f in cat["functions"])
    ran = [a in executed for a in addrs]
    m["executed"] = sum(ran)
    total_exec += m["executed"]
    cells = (len(addrs) + per - 1) // per
    m["cells"] = "".join("1" if any(ran[i * per:(i + 1) * per]) else "0" for i in range(cells))
    print(f"{m['name']:22s} executed {m['executed']:6d} / {len(addrs):6d}")

status["executed_total"] = total_exec
json.dump(status, open(STATUS, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(f"total executed: {total_exec}  -> docs/status.json updated")
