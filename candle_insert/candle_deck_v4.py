# Candle insert v1 - top deck (clips over stock battery box) + snap-in LED arm.  2026-10-10
# v3 2026-10-17: hoop-lip clearance - 45deg corner chamfers (face 27.8 mm from centre),
#   end tabs narrowed 14.2 -> 9 mm (skirt only), board centred across width + capped 36 mm,
#   IR pocket removed (receiver stands on the board), arm socket 1 mm outward (arm part unchanged).
#   (v3 = above with C3_X -16.5, BX0 -12, BRD_L 36 -> candle_deck_v3.stl)
# v4 2026-10-18: C3 wired direct to pads (no headers) -> C3_X -21.5, board start -17, max length 41.
# 1 BU = 1 mm.  Deck coords: z=0 = top of stock battery box (17.2 mm above plate bottom).
import bpy, bmesh, math, os
from mathutils import Vector
OUT = os.path.dirname(os.path.abspath(__file__))                     # renders go next to this script
STL_OUT = os.path.normpath(os.path.join(OUT, "..", "3d_models"))      # STLs go to <repo>/3d_models
# ---- measured / assumed ----
BOX_L, BOX_W = 57.0, 33.0        # stock battery box top (measured)
SHELL_R = 31.4                   # clip radius (rim ID ~63.5 -> 31.75, minus clearance)
CEIL = 46.0 - 17.2               # ceiling above box top = 28.8
SK_T, SK_H, SK_CL = 1.2, 5.0, 0.2  # skirt wall, depth, clearance per side
DECK_T = 2.0
BRD_L, BRD_W = 41.0, 26.5        # v3: far end stays at x 24 (lip clearance); start moved 5 mm left -> max 41
BX0, BY0 = -17.0, -13.25         # v3: board start -17 (was -12), centred across width (BY0 was -12.0)
CHAMF = 27.8                     # v3: corner chamfer face distance from centre (measured on a test print)
TAB_HW = 4.5                     # v3: end-tab half width (tab 9 mm, was ~14.2)
RAIL_X0, RAIL_X1 = 26.0, 31.0    # far-end side guides, measured from BX0 (sit under any board >= 32 mm)
LEDGE = 2.5                      # board raised for solder tails
C3_X = -21.5                     # v3: wires soldered direct to pads (no headers) -> moved 5 mm out (was -16.5)
POST = (-2.0, -14.9)             # arm post position
BULB = (0.0, -2.0)               # bulb centre (near candle centre)
RING_ID, RING_OD = 9.4, 12.0
RING_Z0, RING_Z1 = 16.0, 20.0
def clear():
    for o in list(bpy.data.objects): bpy.data.objects.remove(o, do_unlink=True)
def box(name, x0, x1, y0, y1, z0, z1):
    me = bpy.data.meshes.new(name); bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((x0 if v.co.x < 0 else x1, y0 if v.co.y < 0 else y1, z0 if v.co.z < 0 else z1))
    bm.to_mesh(me); bm.free(); o = bpy.data.objects.new(name, me); bpy.context.collection.objects.link(o); return o
def cyl(name, cx, cy, r, z0, z1, n=96):
    bpy.ops.mesh.primitive_cylinder_add(vertices=n, radius=r, depth=z1 - z0, location=(cx, cy, (z0 + z1) / 2))
    o = bpy.context.active_object; o.name = name; return o
def boolean(t, tool, op):
    m = t.modifiers.new("b", 'BOOLEAN'); m.operation = op; m.object = tool; m.solver = 'EXACT'
    bpy.context.view_layer.objects.active = t
    with bpy.context.temp_override(object=t, active_object=t):
        bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(tool, do_unlink=True)
def nonmanifold(o):
    bm = bmesh.new(); bm.from_mesh(o.data); bad=[e for e in bm.edges if not e.is_manifold]; [print("NM", [tuple(round(c,2) for c in v.co) for v in e.verts]) for e in bad[:8]]; n=len(bad); bm.free(); return n
def export(o, fn):
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active = o
    bpy.ops.wm.stl_export(filepath=os.path.join(STL_OUT, fn), export_selected_objects=True)
    print("EXPORT", fn, tuple(round(d, 1) for d in o.dimensions), "nonmanifold", nonmanifold(o))

def build_deck():
    hx, hy = BOX_L / 2 + SK_CL, BOX_W / 2 + SK_CL          # skirt inner half-sizes
    ox, oy = hx + SK_T, hy + SK_T
    d = box("Deck", -ox, ox, -oy, oy, -SK_H, DECK_T)
    boolean(d, box("in", -hx, hx, -hy, hy, -SK_H - 1, 0), 'DIFFERENCE')         # pocket over box
    boolean(d, cyl("relief", 0, 0, 6.5, -1, DECK_T + 1), 'DIFFERENCE')           # old LED-post stub
    for sx in (-1, 1):                                                          # battery-wire notches
        for sy in (-1, 1):
            boolean(d, box("wn", sx * (ox - 3.5) - 3, sx * (ox - 3.5) + 3 if sx > 0 else sx * (ox - 3.5) + 3,
                           sy * 10 - 3, sy * 10 + 3, -SK_H - 1, DECK_T + 1), 'DIFFERENCE') if False else None
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, x1 = sorted((sx * (ox + 1), sx * (ox - 3)))
            boolean(d, box("wn", x0, x1, sy * 10 - 3, sy * 10 + 3, -SK_H - 1, DECK_T + 1), 'DIFFERENCE')
    # board corner brackets + ledges
    bx0, bx1, by0, by1 = BX0 - 0.3, BX0 + BRD_L + 0.3, BY0 - 0.3, BY0 + BRD_W + 0.3
    top = DECK_T + LEDGE + 1.6 + 0.6
    for sy, cy in ((-1, by0), (1, by1)):                                         # far-end side guides + ledges
        xa, xb = BX0 + RAIL_X0, BX0 + RAIL_X1
        boolean(d, box("Gy", xa, xb, *sorted((cy, cy + sy * 1.2)), DECK_T - 0.01, top), 'UNION')
        boolean(d, box("Gg", xa, xb, *sorted((cy + sy * 0.2, cy - sy * 1.6)), DECK_T - 0.01, DECK_T + LEDGE), 'UNION')
    for cx, sx in ((bx0, -1),):                                                  # row-1 datum corners only
        for cy, sy in ((by0, -1), (by1, 1)):
            boolean(d, box("Lx", *sorted((cx + sx * 1.2, cx - sx * 4.5)), *sorted((cy, cy + sy * 1.2)), DECK_T - 0.01, top), 'UNION')
            boolean(d, box("Ly", *sorted((cx, cx + sx * 1.2)), *sorted((cy, cy - sy * 4.5)), DECK_T - 0.01, top), 'UNION')
            boolean(d, box("Lg", *sorted((cx + sx * 0.2, cx - sx * 3.5)), *sorted((cy + sy * 0.2, cy - sy * 1.6)), DECK_T - 0.01, DECK_T + LEDGE), 'UNION')
    # C3 slot: PCB 1.6 stands on its short edge in plane x=C3_X, y -9..9
    g = 1.9 / 2
    boolean(d, box("c3a", C3_X - g - 1.5, C3_X - g, -6, 6, DECK_T - 0.01, DECK_T + 3), 'UNION')     # pin side (short)
    boolean(d, box("c3b", C3_X + g, C3_X + g + 1.5, -9, 9, DECK_T - 0.01, DECK_T + 2), 'UNION')     # parts side
    # IR receiver pocket at +x end, dome faces +x (outward)
    for wx0, wx1 in ((10.25, 11.25), (17.75, 18.75)):   # IR pocket on -y side, dome faces -y
        pass   # v3 2026-10-17: IR pocket removed - receiver stands up from the board
    # arm socket boss
    px, py = POST[0], POST[1] - 1.0          # v3: socket 1 mm outward to clear centred board (arm part unchanged)
    for c0, c1 in ((px - 3.65, px - 2.15), (px + 2.15, px + 3.65)):
        boolean(d, box("cheek", c0, c1, py - 1.65, py + 1.65, DECK_T - 0.01, DECK_T + 5), 'UNION')
    boolean(d, box("sock", px - 2.15, px + 2.15, py - 1.65, py + 1.65, -1, DECK_T + 6), 'DIFFERENCE')
    # v3: narrow end tabs to 2*TAB_HW - remove skirt wall (below deck only) between tab edge and wire notch
    for sx in (-1, 1):
        x0, x1 = sorted((sx * (ox + 1), sx * (hx - 0.5)))
        for sy in (-1, 1):
            y0, y1 = sorted((sy * TAB_HW, sy * 7.5))
            boolean(d, box("tabcut", x0, x1, y0, y1, -SK_H - 1, 0), 'DIFFERENCE')
    # v3: 45deg corner chamfers - keep inside a square rotated 45deg with apothem CHAMF
    sq = box("chamf", -CHAMF, CHAMF, -CHAMF, CHAMF, -SK_H - 2, CEIL)
    sq.rotation_euler = (0, 0, math.pi / 4); bpy.context.view_layer.update()
    with bpy.context.temp_override(selected_editable_objects=[sq], active_object=sq):
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    boolean(d, sq, 'INTERSECT')
    boolean(d, cyl("clip", 0, 0, SHELL_R, -SK_H - 2, CEIL, 192), 'INTERSECT')   # keep inside shell
    return d

def build_arm():
    px, py = POST; bx, by = BULB
    a = box("Arm", px - 2, px + 2, py - 1.5, py + 1.5, 0, RING_Z1)               # post rests on box top
    ang = math.atan2(by - py, bx - px); L = math.hypot(bx - px, by - py)
    bar = box("bar", 0, L, -2, 2, RING_Z1 - 3, RING_Z1)
    bar.rotation_euler = (0, 0, ang); bar.location = (px, py, 0)
    boolean(a, bar, 'UNION')
    boolean(a, cyl("ring", bx, by, RING_OD / 2, RING_Z0, RING_Z1), 'UNION')
    boolean(a, cyl("ri", bx, by, RING_ID / 2, RING_Z0 - 1, RING_Z1 + 1), 'DIFFERENCE')
    boolean(a, box("wslot", bx - 0.8, bx + 0.8, by - RING_OD / 2 - 1, by, RING_Z0 - 1, RING_Z1 + 1), 'DIFFERENCE')  # wire slot
    return a

clear()
deck = build_deck(); export(deck, "candle_deck_v4.stl")
arm = build_arm()
arm_p = arm.copy(); arm_p.data = arm.data.copy(); bpy.context.collection.objects.link(arm_p)
arm_p.rotation_euler = (math.pi, 0, 0); arm_p.location = (0, 0, RING_Z1)          # print upside-down
bpy.context.view_layer.update()
with bpy.context.temp_override(selected_editable_objects=[arm_p], active_object=arm_p):
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=False)
pass  # v3: arm unchanged, not re-exported
bpy.data.objects.remove(arm_p, do_unlink=True)

# ---- preview with stand-in parts ----
def mat(o, rgba, name):
    m = bpy.data.materials.new(name); m.diffuse_color = rgba; o.data.materials.append(m); return o
mat(deck, (0.85, 0.82, 0.72, 1), "pla"); mat(arm, (0.95, 0.6, 0.2, 1), "arm")
b = box("stockbox", -BOX_L/2, BOX_L/2, -BOX_W/2, BOX_W/2, -17.2, 0); mat(b, (0.97, 0.97, 0.97, 1), "box")
boolean(b, cyl("bc", 0, 0, 32.4, -20, 1, 192), 'INTERSECT'); mat(b, (0.97, 0.97, 0.97, 1), "box2")
pl = cyl("plate", 0, 0, 32.5, -17.2, -15.2, 192); mat(pl, (0.97, 0.97, 0.97, 1), "plate")
z0 = DECK_T + LEDGE
pcb = box("pcb", BX0, BX0 + BRD_L, BY0, BY0 + BRD_W, z0, z0 + 1.6); mat(pcb, (0.05, 0.05, 0.05, 1), "pcb")
r8 = BX0 + 3.67 + 7 * 2.54; ey = BY0 + 3.81
bst = box("boost", r8 - 1.3, r8 - 1.3 + 10.2, ey - 0.8, ey + 0.8, z0 + 1.6 + 1, z0 + 1.6 + 1 + 17.8); mat(bst, (0.1, 0.2, 0.6, 1), "boost")
for r in (1, 5):
    rx = BX0 + 3.67 + (r - 1) * 2.54
    t = box("to92", rx - 0.5, rx + 5.6, ey - 1.5, ey + 2.5, z0 + 1.6 + 3, z0 + 1.6 + 8); mat(t, (0.1, 0.1, 0.1, 1), "to92")
c3 = box("c3", C3_X - 0.8, C3_X + 0.8, -9, 9, DECK_T, DECK_T + 23); mat(c3, (0.1, 0.35, 0.6, 1), "c3")
hp = box("c3pins", C3_X - 0.8 - 3.0, C3_X - 0.8, -9, 9, DECK_T + 2.6, DECK_T + 20.4); mat(hp, (0.2, 0.2, 0.2, 1), "hdr")
bl = cyl("bulb", BULB[0], BULB[1], 4.5, CEIL - 1 - 12, CEIL - 1); mat(bl, (1, 0.95, 0.75, 1), "bulb")
shell = cyl("shell", 0, 0, 31.75, -17.2, CEIL, 192); mat(shell, (1, 0.9, 0.6, 0.15), "wax")
shell.display_type = 'WIRE'; shell.hide_render = True
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_shadows = True; sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 1400, 1000
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
ring = bpy.data.objects.new("ringline", bpy.data.meshes.new("rl")); sc.collection.objects.link(ring)
bm = bmesh.new(); bmesh.ops.create_circle(bm, segments=128, radius=31.75); bm.to_mesh(ring.data); bm.free()
mod = ring.modifiers.new("w", 'WIREFRAME'); mod.thickness = 0.5; mat(ring, (0.7, 0.3, 0.1, 1), "rim")
def shot(fn, loc, tgt, lens=45):
    cam.location = loc; cam.data.lens = lens
    cam.rotation_euler = (Vector(tgt) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = os.path.join(OUT, fn); bpy.ops.render.render(write_still=True); print("RENDER", fn)
for o in (ring,): o.location.z = CEIL
shot("deck_v4_iso.png", (-110, -130, 120), (2, 0, 8), 50)
shot("deck_v4_top.png", (0, 0, 200), (0, 0, 0), 50)
shot("deck_v4_side.png", (0, -200, 6), (0, 0, 6), 50)
print("DONE")
