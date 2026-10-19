# Hub House - 2x ESP32-DevKitC-32E holder. Units: 1 BU = 1 mm.  Generated 2026-10-09
import bpy, bmesh, math, os
from mathutils import Vector
OUT = os.path.dirname(os.path.abspath(__file__))                     # renders go next to this script
STL_OUT = os.path.normpath(os.path.join(OUT, "..", "3d_models"))      # STLs go to <repo>/3d_models
# ---- parameters (edit after test print) ----
L, W, PT = 54.5, 28.5, 1.6      # PCB length, width (stands on edge -> height), thickness
CLR = 0.4                        # length clearance
PARTS, GAP = 4.0, 6.0            # tallest part above PCB, gap between part faces of A and B
PINSIDE = 12.0                   # PCB back -> wall (pins 9 + 3 for soldered wire / bent plug)
T, FL, RAISE = 3.0, 3.0, 4.0     # wall, floor, board lift off floor
GD, GW = 1.5, PT + 0.6           # groove depth into end walls, groove width
COL_T, COL_H, FIT = 1.4, 5.0, 0.3  # collar thickness/height, roof fit clearance
USB_W, USB_H, USB_PROT = 8.5, 13.0, 3.5  # hole (Y x Z), connector height above PCB
SKIRT_H, SHELL = 6.0, 2.0
# ---- derived ----
LIN = L + CLR
XI0, XI1 = T, T + LIN                 # interior X
comp = GAP/2 + PARTS                  # |y| of component face
back = comp + PT                      # |y| of PCB back face
YI = back + PINSIDE                   # interior half width
XO, YO = XI1 + T, YI + T              # exterior length, half width
ZB = FL + RAISE                       # board bottom edge
HB = ZB + W + 3.0                     # body wall height
print("EXTERIOR mm: L=%.1f W=%.1f wallH=%.1f" % (XO, 2*YO, HB))

def clear():
    for o in list(bpy.data.objects): bpy.data.objects.remove(o, do_unlink=True)
def box(name, x0, x1, y0, y1, z0, z1):
    me = bpy.data.meshes.new(name); bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((x0 if v.co.x < 0 else x1, y0 if v.co.y < 0 else y1, z0 if v.co.z < 0 else z1))
    bm.to_mesh(me); bm.free()
    o = bpy.data.objects.new(name, me); bpy.context.collection.objects.link(o); return o
def prism(name, x0, x1, hw, z0, rise):
    me = bpy.data.meshes.new(name); bm = bmesh.new()
    pts = [(-hw, z0), (hw, z0), (0, z0 + rise)]
    a = [bm.verts.new((x0, y, z)) for y, z in pts]; b = [bm.verts.new((x1, y, z)) for y, z in pts]
    bm.faces.new(a[::-1]); bm.faces.new(b)
    for i in range(3):
        j = (i + 1) % 3; bm.faces.new((a[i], a[j], b[j], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces); bm.to_mesh(me); bm.free()
    o = bpy.data.objects.new(name, me); bpy.context.collection.objects.link(o); return o
def boolean(target, tool, op):
    m = target.modifiers.new("b", 'BOOLEAN'); m.operation = op; m.object = tool; m.solver = 'EXACT'
    bpy.context.view_layer.objects.active = target
    with bpy.context.temp_override(object=target, active_object=target):
        bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(tool, do_unlink=True)

def slot(name, xc, yc, zc, sx, sy, sz, rotx=0.0):
    o = box(name, -sx/2, sx/2, -sy/2, sy/2, -sz/2, sz/2)
    o.location = (xc, yc, zc); o.rotation_euler = (rotx, 0, 0); return o
def nonmanifold(o):
    bm = bmesh.new(); bm.from_mesh(o.data)
    n = sum(1 for e in bm.edges if not e.is_manifold); bm.free(); return n

def build_body():
    b = box("Body", 0, XO, -YO, YO, 0, HB)
    boolean(b, box("cav", XI0, XI1, -YI, YI, FL, HB + 1), 'DIFFERENCE')
    col = box("collar", XI0 - COL_T, XI1 + COL_T, -(YI + COL_T), YI + COL_T, HB - 0.05, HB + COL_H)
    boolean(col, box("ci", XI0, XI1, -YI, YI, HB - 1, HB + COL_H + 1), 'DIFFERENCE')
    boolean(b, col, 'UNION')
    top = HB + COL_H + 1
    for s in (-1, 1):
        y0, y1 = sorted((s * (comp - 0.3), s * (back + 0.3)))
        boolean(b, box("gA", XI0 - GD, XI0 + 0.01, y0, y1, ZB, top), 'DIFFERENCE')   # USB-end groove
        boolean(b, box("gB", XI1 - 0.01, XI1 + GD, y0, y1, ZB, top), 'DIFFERENCE')   # far-end groove
        c0, c1 = sorted((s * (comp - USB_PROT - 1.0), s * (comp + 0.3)))
        boolean(b, box("uch", XI0 - 2.0, XI0 + 0.01, c0, c1, ZB + W/2 - USB_H/2 - 1, top), 'DIFFERENCE')
        for i in range(4):                                                         # side vents
            xc = XO * (i + 1) / 5
            boolean(b, slot("sv", xc, s * (YO - T/2), FL + 9 + 9, 3, T + 2, 18), 'DIFFERENCE')
    yc = -(comp - USB_PROT / 2)                                                    # board A only
    boolean(b, slot("usb", T/2, yc, ZB + W/2, T + 4, USB_W, USB_H), 'DIFFERENCE')
    return b

def build_roof():
    r = box("Roof", 0, XO, -YO, YO, 0, SKIRT_H)
    boolean(r, prism("g", 0, XO, YO, SKIRT_H - 0.05, YO), 'UNION')
    hw = YO - SHELL * math.sqrt(2)
    boolean(r, prism("gi", SHELL, XO - SHELL, hw, SKIRT_H - 0.01, hw), 'DIFFERENCE')
    boolean(r, box("si", XI0 - COL_T - FIT, XI1 + COL_T + FIT, -(YI + COL_T + FIT), YI + COL_T + FIT,
                   -1, SKIRT_H + 0.02), 'DIFFERENCE')
    for s in (-1, 1):
        for i in range(5):
            xc = XO * (i + 1) / 6
            boolean(r, slot("rv", xc, s * YO / 2, SKIRT_H + YO / 2, 3, 16, 10, rotx=-s * math.pi / 4), 'DIFFERENCE')
    return r

def export(o, fn):
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True)
    bpy.context.view_layer.objects.active = o
    bpy.ops.wm.stl_export(filepath=os.path.join(STL_OUT, fn), export_selected_objects=True, ascii_format=False)
    print("EXPORT", fn, "dims", tuple(round(d, 1) for d in o.dimensions), "nonmanifold", nonmanifold(o))

clear()
body = build_body(); export(body, "hub_house_body.stl")
roof = build_roof(); export(roof, "hub_house_roof.stl")
test = body.copy(); test.data = body.data.copy(); test.name = "Test"; bpy.context.collection.objects.link(test)
boolean(test, box("tk", -1, XI0 + 14, -YO - 1, YO + 1, -1, HB + COL_H + 1), 'INTERSECT')
export(test, "hub_house_TEST_usb_end.stl")

# ---- preview renders ----
def mat(name, rgba):
    m = bpy.data.materials.new(name); m.diffuse_color = rgba; return m
test.location = (0, -2.3 * YO, 0)
roof.location = (0, 0, HB + 30)
body.data.materials.append(mat("pla", (0.85, 0.80, 0.70, 1)))
roof.data.materials.append(mat("roof", (0.65, 0.25, 0.2, 1)))
test.data.materials.append(mat("test", (0.6, 0.7, 0.85, 1)))
green = mat("pcb", (0.1, 0.55, 0.25, 1))
for s, nm in ((-1, "A"), (1, "B")):
    y0, y1 = sorted((s * comp, s * back))
    p = box("PCB_" + nm, XI0 + CLR/2 - 0, XI0 + CLR/2 + L, y0, y1, ZB, ZB + W); p.data.materials.append(green)
    py0, py1 = sorted((s * back, s * (back + 9)))                         # pin envelope
    pn = box("pins_" + nm, XI0 + 3, XI1 - 3, py0, py1, ZB + W - 2.5, ZB + W); pn.data.materials.append(mat("pin", (0.1, 0.1, 0.1, 1)))
    u0, u1 = sorted((s * comp, s * (comp - USB_PROT)))
    u = box("usb_" + nm, XI0 - 1, XI0 + 7, u0, u1, ZB + W/2 - 4.5, ZB + W/2 + 4.5); u.data.materials.append(mat("usbm", (0.75, 0.75, 0.78, 1)))
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_shadows = True; sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 1400, 1000
sc.world = bpy.data.worlds.new("w") if sc.world is None else sc.world
sc.display.shading.background_type = 'VIEWPORT' if hasattr(sc.display.shading, 'background_type') else None
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
def shot(fn, loc, target, lens=40):
    cam.location = loc; cam.data.lens = lens
    d = Vector(target) - Vector(loc); cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = os.path.join(OUT, fn); bpy.ops.render.render(write_still=True); print("RENDER", fn)
ctr = (XO/2, -0.6 * YO, HB/2 + 10)
shot("preview_iso_usb_end.png", (-150, -190, 170), ctr, 38)
shot("preview_iso_far_end.png", (XO + 150, 160, 170), (XO/2, -0.3 * YO, HB/2 + 10), 38)
shot("preview_usb_wall_closeup.png", (-110, -20, ZB + W/2 + 10), (0, 0, ZB + W/2), 55)
print("DONE")
