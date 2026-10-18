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


RAN, NOTRUN = "#3fb950", "#2d333b"


def num(n, lang):
    return f"{n:,}".replace(",", "." if lang == "pt" else ",")


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def treemap(lang):
    """Per-module map of the lifted functions. A cell is bright only if some
    function in it actually EXECUTED at runtime (coverage run); everything else
    is lifted-but-not-yet-run. Lifting is ~100% - running is the real progress."""
    W, H, PAD, CELL, GAP, TOP, HEAD = 900, 372, 8, 9, 2, 40, 34
    mods, per = S["modules"], S["per_cell"]
    lifted = sum(m["lifted"] for m in mods)
    ran = sum(m.get("executed", 0) for m in mods)
    out = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
           f"<rect width='{W}' height='{H}' rx='8' fill='{BG}'/>"]
    head = ("executadas em runtime" if lang == "pt" else "executed at runtime")
    out.append(f"<text x='{PAD+6}' y='27' {FONT} font-size='15' font-weight='700' fill='{FG}'>"
               f"{head}: <tspan fill='{RAN}'>{num(ran, lang)}</tspan> / {num(lifted, lang)} "
               f"<tspan fill='{MUTED}' font-weight='400'>({pct(ran, lifted):.2f}%)</tspan></text>")
    lx = W - 380
    out.append(f"<rect x='{lx}' y='16' width='10' height='10' rx='2' fill='{RAN}'/>"
               f"<text x='{lx+15}' y='25' {FONT} font-size='11' fill='{MUTED}'>{'executou' if lang=='pt' else 'ran'}</text>"
               f"<rect x='{lx+80}' y='16' width='10' height='10' rx='2' fill='{NOTRUN}' stroke='{BORDER}'/>"
               f"<text x='{lx+95}' y='25' {FONT} font-size='11' fill='{MUTED}'>{'liftada, ainda não rodou' if lang=='pt' else 'lifted, not run yet'}</text>")
    total = sum(len(m.get("cells", "")) or round(m["lifted"] / per) for m in mods)
    x = PAD
    avail = W - PAD * (len(mods) + 1)
    for m in mods:
        cells = m.get("cells") or "0" * max(1, round((m["lifted"] + m["pruned"]) / per))
        w = max(150, avail * len(cells) / total)
        if m is mods[-1]:
            w = W - PAD - x
        y0, h = TOP, H - TOP - PAD
        out.append(f"<rect x='{x:.1f}' y='{y0}' width='{w:.1f}' height='{h}' rx='6' fill='{PANEL}' stroke='{BORDER}'/>")
        ex = m.get("executed", 0)
        sub = f"{num(ex, lang)} / {num(m['lifted'], lang)} ({pct(ex, m['lifted']):.1f}%)"
        out.append(f"<text x='{x+8}' y='{y0+16}' {FONT} font-size='12' font-weight='600' fill='{FG}'>{escape(m['name'])}</text>")
        out.append(f"<text x='{x+8}' y='{y0+29}' {FONT} font-size='10.5' fill='{MUTED}'>{escape(m['kind_' + lang])} · {escape(sub)}</text>")
        cols = max(1, int((w - 16 + GAP) // (CELL + GAP)))
        for i, c in enumerate(cells):
            cx = x + 8 + (i % cols) * (CELL + GAP)
            cy = y0 + HEAD + 6 + (i // cols) * (CELL + GAP)
            if cy + CELL > y0 + h - 6:
                break
            out.append(f"<rect x='{cx:.1f}' y='{cy}' width='{CELL}' height='{CELL}' rx='1.5' fill='{RAN if c == '1' else NOTRUN}'/>")
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
ran = sum(m.get("executed", 0) for m in S["modules"])
p = pct(ran, fns)
write("badge-functions.svg", badge("functions lifted", f"{fns:,}", "#0969da"))
for lang in ("en", "pt"):
    write(f"badge-executed-{lang}.svg",
          badge("executadas em runtime" if lang == "pt" else "executed at runtime",
                f"{p:.2f}%".replace(".", ",") if lang == "pt" else f"{p:.2f}%",
                "#cf222e" if p < 5 else "#bf8700" if p < 50 else "#2ea043"))
write("badge-failures.svg", badge("lift failures", str(S["lift_failures"]), "#2ea043" if S["lift_failures"] == 0 else "#d73a49"))
write("badge-imports.svg", badge("imports", f"{S['imports_resolved']}/{S['imports_total']}", "#2ea043"))
for lang in ("en", "pt"):
    write(f"badge-phase-{lang}.svg", badge("fase" if lang == "pt" else "phase", S["phase_" + lang], "#bf8700"))
    write(f"progress-{lang}.svg", treemap(lang))
    write(f"milestones-{lang}.svg", milestones(lang))
print("status images written to docs/")
