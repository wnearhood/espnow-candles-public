# candle_deck_v5.py - 2026-10-18
# v5 = v3 layout (v3 fit tested best) + end section narrowed 14 -> 12 mm so the deck
# sits between the stock battery terminals (gap measured 14.05). Hanging tabs stay 9 mm.
# Built from candle_deck_v4.py by checked text substitution (each must hit exactly once).
# Run: blender.exe -b --python candle_deck_v5.py
import os
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "candle_deck_v4.py")
src = open(SRC, encoding="utf-8").read()
SUBS = [
    # back to v3 layout
    ("C3_X = -21.5", "C3_X = -16.5"),
    ("BRD_L, BRD_W = 41.0, 26.5", "BRD_L, BRD_W = 36.0, 26.5"),
    ("BX0, BY0 = -17.0, -13.25", "BX0, BY0 = -12.0, -13.25"),
    ("C3_X - 0.8 - 3.0", "C3_X - 0.8 - 3.0"),  # keep 3 mm pad-side preview (no headers)
    # end section 14 -> 12 mm: wire-notch inner edge y 7 -> 6 (deck + skirt, both ends)
    ("boolean(d, box(\"wn\", x0, x1, sy * 10 - 3, sy * 10 + 3,",
     "boolean(d, box(\"wn\", x0, x1, *sorted((sy * END_HW, sy * 13)),"),
    # outputs
    ("candle_deck_v4.stl", "candle_deck_v5.stl"),
]
for old, new in SUBS:
    n = src.count(old)
    assert n == 1, f"substitution hit {n}x (expected 1): {old!r}"
    src = src.replace(old, new)
n = src.count("deck_v4_")
assert n == 3, f"render names hit {n}x (expected 3)"
src = src.replace("deck_v4_", "deck_v5_")
src = src.replace("TAB_HW = 4.5", "TAB_HW = 4.5\nEND_HW = 6.0                     # v5: end section half width (12 mm)", 1)
assert "END_HW = 6.0" in src
exec(compile(src, "candle_deck_v5(from v4)", "exec"))
