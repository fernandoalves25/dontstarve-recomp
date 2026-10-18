#!/usr/bin/env python3
"""Generate the README status images from docs/status.json.

Writes SVGs into docs/ (rendered inline by GitHub, like AnyPS5's progress map):
  badge-functions.svg, badge-failures.svg, badge-phase-{en,pt}.svg,
  progress-{en,pt}.svg   (treemap of lifted functions per module)
  milestones-{en,pt}.svg (M0..M9 strip)
Run after updating status.json:  python scripts/gen_status.py
"""
import json, os
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.join(HERE, "..", "docs")
S = json.load(open(os.path.join(DOCS, "status.json"), encoding="utf-8"))

BG, PANEL, BORDER, FG, MUTED = "#0d1117", "#161b22", "#30363d", "#e6edf3", "#8b949e"
GREEN, GRID, PRUNED, AMBER = "#3fb950", "#2ea043", "#6e7681", "#d29922"
FONT = "font-family='-apple-system,Segoe UI,Helvetica,Arial,sans-serif'"


def write(name, svg):
    with open(os.path.join(DOCS, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)


def badge(label, value, color):
    """shields-style flat badge; widths estimated from text length."""
    lw, vw = 7 * len(label) + 12, 7 * len(value) + 12
    w = lw + vw
    return (f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='20' role='img' aria-label='{escape(label)}: {escape(value)}'>"
            f"<rect width='{lw}' height='20' fill='#555'/><rect x='{lw}' width='{vw}' height='20' fill='{color}'/>"
            f"<g fill='#fff' {FONT} font-size='11'>"
            f"<text x='{lw/2}' y='14' text-anchor='middle'>{escape(label)}</text>"
            f"<text x='{lw + vw/2}' y='14' text-anchor='middle'>{escape(value)}</text></g></svg>")


def treemap(lang):
    W, H, PAD, CELL, GAP, HEAD = 900, 330, 8, 9, 2, 34
    mods, per = S["modules"], S["per_cell"]
    total = sum(m["lifted"] + m["pruned"] for m in mods)
    out = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
           f"<rect width='{W}' height='{H}' rx='8' fill='{BG}'/>"]
    x = PAD
    avail = W - PAD * (len(mods) + 1)
    for m in mods:
        n = m["lifted"] + m["pruned"]
        w = max(150, avail * n / total)
        if m is mods[-1]:
            w = W - PAD - x
        h = H - 2 * PAD
        out.append(f"<rect x='{x}' y='{PAD}' width='{w:.1f}' height='{h}' rx='6' fill='{PANEL}' stroke='{BORDER}'/>")
        kind = m["kind_" + lang]
        sub = f"{m['lifted']:,} fns".replace(",", "." if lang == "pt" else ",")
        if m["pruned"]:
            sub += f" · {m['pruned']} " + ("podadas" if lang == "pt" else "pruned")
        out.append(f"<text x='{x+8}' y='{PAD+16}' {FONT} font-size='12' font-weight='600' fill='{FG}'>{escape(m['name'])}</text>")
        out.append(f"<text x='{x+8}' y='{PAD+29}' {FONT} font-size='10.5' fill='{MUTED}'>{escape(kind)} · {escape(sub)}</text>")
        cols = max(1, int((w - 16 + GAP) // (CELL + GAP)))
        nl, np_ = max(1, round(m["lifted"] / per)), round(m["pruned"] / per)
        for i in range(nl + np_):
            cx = x + 8 + (i % cols) * (CELL + GAP)
            cy = PAD + HEAD + 6 + (i // cols) * (CELL + GAP)
            if cy + CELL > PAD + h - 6:
                break
            out.append(f"<rect x='{cx:.1f}' y='{cy}' width='{CELL}' height='{CELL}' rx='1.5' fill='{GRID if i < nl else PRUNED}'/>")
        x += w + PAD
    out.append("</svg>")
    return "".join(out)


def milestones(lang):
    ms = S["milestones"]
    W, H, PAD = 900, 74, 8
    cw = (W - PAD * (len(ms) + 1)) / len(ms)
    col = {"ok": GREEN, "work": AMBER, "todo": BORDER}
    out = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
           f"<rect width='{W}' height='{H}' rx='8' fill='{BG}'/>"]
    for i, m in enumerate(ms):
        x = PAD + i * (cw + PAD)
        c = col[m["state"]]
        op = "1" if m["state"] != "todo" else "0.6"
        out.append(f"<g opacity='{op}'><rect x='{x:.1f}' y='{PAD}' width='{cw:.1f}' height='{H-2*PAD}' rx='6' fill='{PANEL}' stroke='{BORDER}'/>"
                   f"<rect x='{x:.1f}' y='{PAD}' width='{cw:.1f}' height='4' rx='2' fill='{c}'/>"
                   f"<text x='{x+cw/2:.1f}' y='{PAD+28}' text-anchor='middle' {FONT} font-size='13' font-weight='700' fill='{c if m['state']!='todo' else MUTED}'>{m['id']}</text>"
                   f"<text x='{x+cw/2:.1f}' y='{PAD+46}' text-anchor='middle' {FONT} font-size='10' fill='{FG}'>{escape(m[lang])}</text></g>")
    out.append("</svg>")
    return "".join(out)


fns = sum(m["lifted"] for m in S["modules"])
write("badge-functions.svg", badge("functions lifted", f"{fns:,}", "#2ea043"))
write("badge-failures.svg", badge("lift failures", str(S["lift_failures"]), "#2ea043" if S["lift_failures"] == 0 else "#d73a49"))
write("badge-imports.svg", badge("imports", f"{S['imports_resolved']}/{S['imports_total']}", "#2ea043"))
for lang in ("en", "pt"):
    write(f"badge-phase-{lang}.svg", badge("fase" if lang == "pt" else "phase", S["phase_" + lang], "#bf8700"))
    write(f"progress-{lang}.svg", treemap(lang))
    write(f"milestones-{lang}.svg", milestones(lang))
print("status images written to docs/")
