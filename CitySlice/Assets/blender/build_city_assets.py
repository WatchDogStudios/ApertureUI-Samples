"""build_city_assets.py - generates CitySlice's model library in Blender.

Run headless:
    blender --background --python build_city_assets.py

Writes Assets/models/city_assets.glb: one object per asset, named so the game
can look it up (see kAssetNames in CityAssets.cpp). Everything is authored
parametrically so the library stays editable and reproducible -- open the .blend
this writes alongside the .glb to tweak by hand, or edit the parameters here.

Conventions the loader depends on:
  * +Z up in Blender; the glTF exporter converts to +Y up, which is world space.
  * Origin at the FOOTPRINT CENTRE, on the ground (z = 0), so the game places a
    model by its tile centre and it stands on the terrain.
  * The building's street-facing side is -Y in Blender (the game rotates the
    model to face its road).
  * Material NAMES carry the shading id (brick, plaster, concrete, roof,
    window, metal, paint, foliage, emissive, road, dirt). The loader maps the
    name to the engine's material byte and takes the base colour from the
    material, so nothing here needs UVs or textures.
  * Metric units: a tile is 8 m, so keep footprints under ~7 m.
"""
import math
import os
import sys

import bpy
import bmesh
from mathutils import Vector

# --------------------------------------------------------------------------
# Materials: name -> (base colour, roughness, metallic). The name is what the
# game reads; roughness/metallic are for looking at the library in Blender.
# --------------------------------------------------------------------------
MATERIALS = {
    "brick":    ((0.62, 0.34, 0.28, 1), 0.90, 0.0),
    "brick_pale": ((0.78, 0.62, 0.50, 1), 0.90, 0.0),
    "plaster":  ((0.88, 0.84, 0.75, 1), 0.85, 0.0),
    "plaster_warm": ((0.85, 0.72, 0.55, 1), 0.85, 0.0),
    "concrete": ((0.70, 0.69, 0.67, 1), 0.80, 0.0),
    "roof":     ((0.34, 0.18, 0.16, 1), 0.85, 0.0),
    "roof_slate": ((0.26, 0.27, 0.30, 1), 0.75, 0.0),
    "roof_metal": ((0.58, 0.59, 0.62, 1), 0.45, 0.8),
    "window":   ((0.42, 0.55, 0.66, 1), 0.08, 0.0),
    "metal":    ((0.64, 0.66, 0.70, 1), 0.40, 0.9),
    "paint":    ((0.80, 0.80, 0.82, 1), 0.25, 0.1),
    "paint_red": ((0.72, 0.20, 0.17, 1), 0.25, 0.1),
    "paint_blue": ((0.16, 0.32, 0.72, 1), 0.25, 0.1),
    "paint_yellow": ((0.90, 0.72, 0.18, 1), 0.25, 0.1),
    "foliage":  ((0.24, 0.48, 0.20, 1), 0.80, 0.0),
    "foliage_dark": ((0.16, 0.36, 0.20, 1), 0.80, 0.0),
    "bark":     ((0.32, 0.22, 0.14, 1), 0.90, 0.0),
    "emissive": ((1.00, 0.90, 0.70, 1), 0.50, 0.0),
    "emissive_red": ((1.00, 0.25, 0.20, 1), 0.50, 0.0),
    "road":     ((0.24, 0.24, 0.26, 1), 0.75, 0.0),
    "dirt":     ((0.45, 0.38, 0.28, 1), 0.95, 0.0),
    "grass":    ((0.34, 0.52, 0.24, 1), 0.95, 0.0),
    "glass_dark": ((0.20, 0.26, 0.32, 1), 0.10, 0.0),
    "wood":     ((0.48, 0.32, 0.18, 1), 0.80, 0.0),
}


def ensure_materials():
    for name, (colour, rough, metal) in MATERIALS.items():
        mat = bpy.data.materials.get(name)
        if mat is None:
            mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            bsdf.inputs["Base Color"].default_value = colour
            bsdf.inputs["Roughness"].default_value = rough
            bsdf.inputs["Metallic"].default_value = metal
            if name.startswith("emissive"):
                bsdf.inputs["Emission Color"].default_value = colour
                bsdf.inputs["Emission Strength"].default_value = 2.0
        mat.diffuse_color = colour


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for block in (bpy.data.meshes, bpy.data.materials, bpy.data.objects):
        for item in list(block):
            block.remove(item)


# --------------------------------------------------------------------------
# Builder: accumulates bmesh geometry with per-face materials, then bakes one
# object. Every asset is built through this so materials stay consistent.
# --------------------------------------------------------------------------
class Builder:
    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.slots = []          # material names, in slot order
        self._slot_of = {}

    def slot(self, material):
        if material not in self._slot_of:
            self._slot_of[material] = len(self.slots)
            self.slots.append(material)
        return self._slot_of[material]

    def _tag(self, faces, material):
        idx = self.slot(material)
        for f in faces:
            f.material_index = idx

    # ---- primitives -------------------------------------------------------
    def box(self, size, at=(0, 0, 0), material="concrete", bevel=0.0, segments=1):
        """Box of `size` (x, y, z) with its BOTTOM centre at `at`."""
        sx, sy, sz = size
        res = bmesh.ops.create_cube(self.bm, size=1.0)
        faces = [f for f in res["verts"][0].link_faces] if res["verts"] else []
        verts = res["verts"]
        bmesh.ops.scale(self.bm, vec=(sx, sy, sz), verts=verts)
        bmesh.ops.translate(self.bm, vec=(at[0], at[1], at[2] + sz * 0.5), verts=verts)
        faces = set()
        for v in verts:
            faces.update(v.link_faces)
        faces = list(faces)
        if bevel > 0.0:
            edges = set()
            for f in faces:
                edges.update(f.edges)
            out = bmesh.ops.bevel(self.bm, geom=list(edges), offset=bevel,
                                  segments=segments, affect="EDGES", profile=0.6,
                                  clamp_overlap=True)
            faces = [f for f in out["faces"]] + faces
        self._tag([f for f in faces if f.is_valid], material)
        return faces

    def cylinder(self, radius, height, at=(0, 0, 0), material="metal", segments=12,
                 cap=True, radius_top=None):
        res = bmesh.ops.create_cone(self.bm, cap_ends=cap, cap_tris=False,
                                    segments=segments,
                                    radius1=radius,
                                    radius2=radius if radius_top is None else radius_top,
                                    depth=height)
        verts = res["verts"]
        bmesh.ops.translate(self.bm, vec=(at[0], at[1], at[2] + height * 0.5), verts=verts)
        faces = set()
        for v in verts:
            faces.update(v.link_faces)
        self._tag([f for f in faces if f.is_valid], material)
        return list(faces)

    def sphere(self, radius, at=(0, 0, 0), material="foliage", subdiv=2):
        res = bmesh.ops.create_icosphere(self.bm, subdivisions=subdiv, radius=radius)
        verts = res["verts"]
        bmesh.ops.translate(self.bm, vec=at, verts=verts)
        faces = set()
        for v in verts:
            faces.update(v.link_faces)
        self._tag([f for f in faces if f.is_valid], material)
        return list(faces)

    def prism(self, points, height, at=(0, 0, 0), material="concrete"):
        """Extruded polygon: `points` is a list of (x, y) in CCW order."""
        vs = [self.bm.verts.new((at[0] + x, at[1] + y, at[2])) for x, y in points]
        face = self.bm.faces.new(vs)
        res = bmesh.ops.extrude_face_region(self.bm, geom=[face])
        new_verts = [e for e in res["geom"] if isinstance(e, bmesh.types.BMVert)]
        bmesh.ops.translate(self.bm, vec=(0, 0, height), verts=new_verts)
        faces = set([face])
        for v in vs + new_verts:
            faces.update(v.link_faces)
        self._tag([f for f in faces if f.is_valid], material)
        return list(faces)

    def slab(self, corners, thickness, material="concrete"):
        """Slanted slab from four 3D corners (CCW), given thickness downwards.
        Used for awnings and canopies that a box cannot express."""
        top = [self.bm.verts.new(c) for c in corners]
        bottom = [self.bm.verts.new((c[0], c[1], c[2] - thickness)) for c in corners]
        made = [self.bm.faces.new(top), self.bm.faces.new(list(reversed(bottom)))]
        for i in range(4):
            j = (i + 1) % 4
            made.append(self.bm.faces.new([top[i], top[j], bottom[j], bottom[i]]))
        self._tag(made, material)
        return made

    def gable_roof(self, width, depth, eave_z, ridge_h, at=(0, 0, 0), material="roof",
                   overhang=0.25, along_x=True):
        hw, hd = width * 0.5 + overhang, depth * 0.5 + overhang
        x, y, z = at[0], at[1], at[2] + eave_z
        if along_x:
            pts = [(-hw, -hd, 0), (hw, -hd, 0), (hw, hd, 0), (-hw, hd, 0),
                   (-hw, 0, ridge_h), (hw, 0, ridge_h)]
            faces_idx = [(0, 1, 5, 4), (2, 3, 4, 5), (0, 4, 3), (1, 2, 5)]
        else:
            pts = [(-hw, -hd, 0), (hw, -hd, 0), (hw, hd, 0), (-hw, hd, 0),
                   (0, -hd, ridge_h), (0, hd, ridge_h)]
            faces_idx = [(0, 4, 5, 3), (1, 2, 5, 4), (0, 1, 4), (2, 3, 5)]
        vs = [self.bm.verts.new((x + p[0], y + p[1], z + p[2])) for p in pts]
        made = []
        for idx in faces_idx:
            try:
                made.append(self.bm.faces.new([vs[i] for i in idx]))
            except ValueError:
                pass
        # thin slab under the roof so the eave has thickness from below
        self._tag(made, material)
        return made

    def hip_roof(self, width, depth, eave_z, height, inset, at=(0, 0, 0),
                 material="roof", overhang=0.25):
        hw, hd = width * 0.5 + overhang, depth * 0.5 + overhang
        tw, td = max(0.05, hw - inset), max(0.05, hd - inset)
        x, y, z = at[0], at[1], at[2] + eave_z
        low = [(-hw, -hd), (hw, -hd), (hw, hd), (-hw, hd)]
        high = [(-tw, -td), (tw, -td), (tw, td), (-tw, td)]
        lv = [self.bm.verts.new((x + p[0], y + p[1], z)) for p in low]
        hv = [self.bm.verts.new((x + p[0], y + p[1], z + height)) for p in high]
        made = []
        for i in range(4):
            j = (i + 1) % 4
            made.append(self.bm.faces.new([lv[i], lv[j], hv[j], hv[i]]))
        made.append(self.bm.faces.new(hv))
        self._tag(made, material)
        return made

    def window_row(self, count, y, z, width, height, span, material="window",
                   depth=0.06, frame="concrete", frame_pad=0.07):
        """A row of panes proud of a wall facing -Y (the street side)."""
        if count <= 0:
            return
        for i in range(count):
            x = -span * 0.5 + span * (i + 0.5) / count
            self.box((width + frame_pad * 2, depth * 0.6, height + frame_pad * 2),
                     at=(x, y + depth * 0.3, z - (height + frame_pad * 2) * 0.5),
                     material=frame)
            self.box((width, depth, height), at=(x, y, z - height * 0.5),
                     material=material)

    def window_grid(self, floors, per_floor, y, z0, dz, width, height, span,
                    material="window", **kw):
        for f in range(floors):
            self.window_row(per_floor, y, z0 + f * dz, width, height, span,
                            material=material, **kw)

    # ---- bake -------------------------------------------------------------
    def finish(self, smooth_angle=None):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=1e-4)
        mesh = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(mesh)
        self.bm.free()
        obj = bpy.data.objects.new(self.name, mesh)
        for slot_name in self.slots:
            mesh.materials.append(bpy.data.materials[slot_name])
        bpy.context.collection.objects.link(obj)
        if smooth_angle is not None:
            for poly in mesh.polygons:
                poly.use_smooth = True
            mod = obj.modifiers.new("Smooth", "EDGE_SPLIT")
            mod.split_angle = math.radians(smooth_angle)
        return obj


# --------------------------------------------------------------------------
# Residential
# --------------------------------------------------------------------------
def asset_res_house():
    b = Builder("res_house")
    w, d, h = 4.6, 3.8, 3.0
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="grass")
    b.box((w + 0.3, d + 0.3, 0.35), at=(0, 0, 0), material="concrete", bevel=0.04)
    b.box((w, d, h), at=(0, 0, 0.3), material="brick", bevel=0.06, segments=2)
    b.gable_roof(w, d, 3.3, 1.6, material="roof", overhang=0.35, along_x=True)
    b.window_row(2, -d * 0.5 - 0.02, 2.3, 0.95, 1.15, 3.0)
    b.window_row(2, d * 0.5 + 0.02, 2.3, 0.95, 1.15, 3.0)
    # door + porch
    b.box((1.0, 0.12, 2.05), at=(0, -d * 0.5 - 0.04, 0.3), material="wood")
    b.box((1.9, 0.9, 0.14), at=(0, -d * 0.5 - 0.45, 2.45), material="wood", bevel=0.03)
    for sx in (-0.8, 0.8):
        b.cylinder(0.07, 2.45, at=(sx, -d * 0.5 - 0.8, 0.3), material="plaster", segments=8)
    b.box((0.55, 0.55, 2.4), at=(w * 0.4, d * 0.25, 3.2), material="brick", bevel=0.04)
    # front path and hedge
    b.box((1.1, 2.6, 0.05), at=(0, -d * 0.5 - 1.6, 0.0), material="concrete")
    b.box((6.4, 0.45, 0.8), at=(0, -3.2, 0), material="foliage", bevel=0.12, segments=2)
    return b.finish(smooth_angle=40)


def asset_res_townhouse():
    b = Builder("res_townhouse")
    w, d, h = 5.4, 4.6, 6.4
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="grass")
    b.box((w + 0.4, d + 0.4, 0.45), at=(0, 0, 0), material="concrete", bevel=0.05)
    b.box((w, d, h), at=(0, 0, 0.4), material="plaster_warm", bevel=0.08, segments=2)
    # string course between floors and a cornice
    b.box((w + 0.22, d + 0.22, 0.18), at=(0, 0, 3.5), material="concrete")
    b.box((w + 0.30, d + 0.30, 0.26), at=(0, 0, 6.55), material="concrete", bevel=0.04)
    b.hip_roof(w, d, 6.85, 1.5, 1.3, material="roof_slate", overhang=0.35)
    for z in (2.2, 5.3):
        b.window_row(3, -d * 0.5 - 0.02, z, 0.95, 1.3, 4.0)
        b.window_row(3, d * 0.5 + 0.02, z, 0.95, 1.3, 4.0)
    b.box((1.1, 0.14, 2.2), at=(0, -d * 0.5 - 0.05, 0.4), material="wood")
    # balcony
    b.box((2.6, 0.9, 0.16), at=(0, -d * 0.5 - 0.4, 3.4), material="concrete", bevel=0.03)
    b.box((2.6, 0.06, 0.9), at=(0, -d * 0.5 - 0.82, 3.56), material="metal")
    b.box((0.5, 0.5, 2.0), at=(-w * 0.32, d * 0.3, 6.8), material="brick", bevel=0.04)
    return b.finish(smooth_angle=40)


def asset_res_apartment():
    b = Builder("res_apartment")
    w, d, h = 6.2, 5.6, 15.5
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w + 0.5, d + 0.5, 0.55), at=(0, 0, 0), material="concrete", bevel=0.06)
    b.box((w, d, h), at=(0, 0, 0.5), material="brick_pale", bevel=0.14, segments=2)
    for f in range(5):
        z = 2.3 + f * 3.0
        b.window_row(4, -d * 0.5 - 0.02, z, 0.9, 1.5, 5.0)
        b.window_row(4, d * 0.5 + 0.02, z, 0.9, 1.5, 5.0)
        if f:  # balcony ledges
            b.box((w + 0.35, 0.55, 0.14), at=(0, -d * 0.5 - 0.2, z - 1.1), material="concrete")
            b.box((w + 0.35, 0.05, 0.55), at=(0, -d * 0.5 - 0.45, z - 0.96), material="metal")
    b.box((1.8, 0.16, 2.5), at=(0, -d * 0.5 - 0.05, 0.5), material="glass_dark")
    b.box((2.6, 1.1, 0.18), at=(0, -d * 0.5 - 0.5, 3.0), material="metal", bevel=0.03)
    b.box((w + 0.35, d + 0.35, 0.30), at=(0, 0, h + 0.2), material="concrete", bevel=0.04)
    b.box((w + 0.15, d + 0.15, 0.70), at=(0, 0, h + 0.5), material="brick_pale")
    b.box((2.4, 2.0, 1.8), at=(-1.2, 0.9, h + 0.5), material="concrete", bevel=0.06)
    b.cylinder(0.55, 1.3, at=(1.7, -1.3, h + 0.5), material="metal", segments=14)
    return b.finish(smooth_angle=40)


# --------------------------------------------------------------------------
# Commercial
# --------------------------------------------------------------------------
def asset_com_shop():
    b = Builder("com_shop")
    w, d, h = 6.2, 5.0, 4.4
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w, d, h), at=(0, 0, 0), material="plaster", bevel=0.10, segments=2)
    b.box((w + 0.30, d + 0.30, 0.35), at=(0, 0, h - 0.1), material="concrete", bevel=0.04)
    b.box((w + 0.16, d + 0.16, 0.55), at=(0, 0, h + 0.25), material="plaster")
    # storefront glazing
    b.box((5.2, 0.14, 2.4), at=(-0.4, -d * 0.5 - 0.03, 0.35), material="window")
    b.box((1.0, 0.16, 2.4), at=(2.4, -d * 0.5 - 0.04, 0.3), material="glass_dark")
    # striped awning: slanted slabs, alternating colours
    y0 = -d * 0.5 - 0.02
    for i in range(6):
        x = -2.85 + i * 0.95
        mat = "paint_red" if i % 2 == 0 else "paint"
        b.slab([(x, y0, 3.25), (x + 0.95, y0, 3.25),
                (x + 0.95, y0 - 1.15, 2.70), (x, y0 - 1.15, 2.70)], 0.07, material=mat)
    b.box((4.4, 0.12, 0.75), at=(0, -d * 0.5 - 0.10, 3.45), material="emissive")
    b.box((2.0, 1.6, 0.8), at=(1.6, 1.4, h + 0.55), material="metal", bevel=0.05)
    b.window_row(2, d * 0.5 + 0.02, 3.0, 0.9, 0.9, 3.4)
    return b.finish(smooth_angle=40)


def asset_com_office():
    b = Builder("com_office")
    w, d, h = 6.2, 6.0, 11.5
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w, d, h), at=(0, 0, 0), material="concrete", bevel=0.16, segments=2)
    # continuous glass bands between spandrels
    for f in range(3):
        z = 1.2 + f * 3.4
        b.box((w + 0.10, d + 0.10, 2.2), at=(0, 0, z), material="window")
    b.box((2.6, 0.18, 2.6), at=(0, -d * 0.5 - 0.05, 0), material="glass_dark")
    b.box((3.4, 1.4, 0.22), at=(0, -d * 0.5 - 0.7, 3.0), material="metal", bevel=0.04)
    b.box((w + 0.34, d + 0.34, 0.34), at=(0, 0, h - 0.15), material="concrete", bevel=0.04)
    b.box((2.6, 2.2, 1.4), at=(1.2, -1.2, h + 0.19), material="concrete", bevel=0.06)
    b.cylinder(0.5, 1.6, at=(-1.6, 1.2, h + 0.19), material="metal", segments=14)
    return b.finish(smooth_angle=40)


def asset_com_tower():
    b = Builder("com_tower")
    w, d, h = 6.2, 6.2, 26.0
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w + 0.4, d + 0.4, 0.6), at=(0, 0, 0), material="concrete", bevel=0.06)
    b.box((w, d, h), at=(0, 0, 0.55), material="window", bevel=0.30, segments=2)
    # mullions
    for i in range(-2, 3):
        b.box((0.16, d + 0.34, h - 1.0), at=(i * 1.5, 0, 0.9), material="metal")
    for i in range(-2, 3):
        b.box((w + 0.34, 0.16, h - 1.0), at=(0, i * 1.5, 0.9), material="metal")
    b.box((3.0, 0.2, 3.2), at=(0, -d * 0.5 - 0.08, 0.55), material="glass_dark")
    b.box((4.2, 1.8, 0.24), at=(0, -d * 0.5 - 0.9, 3.7), material="metal", bevel=0.04)
    # crown + spire
    b.box((w - 1.6, d - 1.6, 4.0), at=(0, 0, h + 0.55), material="window", bevel=0.25)
    b.box((w - 1.2, d - 1.2, 0.35), at=(0, 0, h + 4.4), material="concrete", bevel=0.05)
    b.cylinder(0.22, 6.0, at=(0, 0, h + 4.7), material="metal", segments=10, radius_top=0.06)
    b.sphere(0.34, at=(0, 0, h + 10.9), material="emissive_red", subdiv=2)
    return b.finish(smooth_angle=35)


# --------------------------------------------------------------------------
# Industrial
# --------------------------------------------------------------------------
def asset_ind_warehouse():
    b = Builder("ind_warehouse")
    w, d, h = 6.4, 5.4, 3.8
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="road")
    b.box((w, d, h), at=(0, 0, 0), material="metal", bevel=0.06)
    # sawtooth roof
    for i in range(2):
        y = -d * 0.25 + i * d * 0.5
        b.gable_roof(w, d * 0.5, h, 1.1, at=(0, y, 0), material="roof_metal",
                     overhang=0.12, along_x=True)
    b.box((2.8, 0.16, 2.9), at=(0, -d * 0.5 - 0.04, 0), material="paint_blue")
    for i in range(3):
        b.box((0.9, 0.9, 0.9), at=(-2.0 + i * 1.9, -3.0, 0), material="wood", bevel=0.04)
    b.box((6.8, 0.08, 1.3), at=(0, 3.3, 0), material="metal")
    return b.finish(smooth_angle=40)


def asset_ind_factory():
    b = Builder("ind_factory")
    w, d, h = 6.6, 6.0, 5.6
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="road")
    b.box((w, d, h), at=(0, 0, 0), material="plaster", bevel=0.08)
    b.gable_roof(w, d, h, 1.4, material="roof_metal", overhang=0.22, along_x=True)
    b.box((2.6, 0.16, 3.0), at=(-1.2, -d * 0.5 - 0.04, 0), material="metal")
    b.window_row(4, -d * 0.5 - 0.02, 4.4, 0.85, 0.8, 5.2, material="glass_dark")
    b.cylinder(0.55, 10.5, at=(2.4, 2.2, 0), material="brick", segments=14, radius_top=0.42)
    b.cylinder(0.52, 0.5, at=(2.4, 2.2, 10.5), material="metal", segments=14)
    b.box((0.36, 5.6, 0.36), at=(-w * 0.5 - 0.3, 0, 2.2), material="metal", bevel=0.05)
    b.box((1.6, 1.6, 0.9), at=(-1.4, 0.6, h + 1.4), material="metal", bevel=0.05)
    return b.finish(smooth_angle=40)


def asset_ind_plant():
    b = Builder("ind_plant")
    w, d, h = 6.8, 6.4, 8.0
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="road")
    b.box((w, d, h), at=(0, 0, 0), material="metal", bevel=0.10)
    b.gable_roof(w, d, h, 1.2, material="roof_metal", overhang=0.22, along_x=True)
    b.cylinder(0.60, 14.0, at=(2.3, 2.3, 0), material="brick", segments=14, radius_top=0.45)
    b.cylinder(0.50, 12.0, at=(-2.3, 2.3, 0), material="brick", segments=14, radius_top=0.40)
    b.sphere(0.30, at=(2.3, 2.3, 14.3), material="emissive_red", subdiv=1)
    b.cylinder(0.95, 3.2, at=(-2.4, -2.4, 0), material="metal", segments=16)
    b.cylinder(0.75, 2.6, at=(-0.4, -2.6, 0), material="paint_yellow", segments=16)
    # conveyor gantry
    b.box((0.5, 7.2, 0.4), at=(w * 0.5 + 0.4, 0, 3.0), material="metal", bevel=0.05)
    for y in (-2.8, 2.8):
        b.box((0.24, 0.24, 3.0), at=(w * 0.5 + 0.4, y, 0), material="metal")
    b.window_row(5, -d * 0.5 - 0.02, 6.4, 0.85, 0.8, 5.4, material="glass_dark")
    return b.finish(smooth_angle=40)


# --------------------------------------------------------------------------
# Services
# --------------------------------------------------------------------------
def asset_svc_power():
    b = Builder("svc_power")
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="road")
    b.box((5.2, 3.2, 5.5), at=(-0.9, 1.6, 0), material="metal", bevel=0.10)
    b.window_row(4, 0.0 - 0.02, 3.8, 0.85, 0.9, 4.2, material="glass_dark")
    b.box((2.2, 0.16, 3.0), at=(-0.5, -0.02, 0), material="paint_yellow")
    # cooling towers (hyperbolic-ish: two stacked cones)
    for (x, y, r, hh) in ((1.9, -1.6, 1.75, 7.0), (-2.2, -1.8, 1.35, 5.5)):
        b.cylinder(r, hh, at=(x, y, 0), material="concrete", segments=18, radius_top=r * 0.66)
        b.cylinder(r * 0.66, hh * 0.3, at=(x, y, hh), material="concrete",
                   segments=18, radius_top=r * 0.74)
    b.cylinder(0.45, 15.0, at=(-2.9, 2.6, 0), material="brick", segments=12, radius_top=0.35)
    b.sphere(0.28, at=(-2.9, 2.6, 15.3), material="emissive_red", subdiv=1)
    for i in range(3):
        b.box((0.6, 0.6, 1.4), at=(2.2 - i * 0.9, 2.9, 0), material="metal", bevel=0.04)
    return b.finish(smooth_angle=35)


def asset_svc_water():
    b = Builder("svc_water")
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="grass")
    # tank on four legs
    for sx in (-1.5, 1.5):
        for sy in (-1.5, 1.5):
            b.cylinder(0.13, 8.5, at=(sx, sy - 0.6, 0), material="metal", segments=8)
    b.box((3.2, 3.2, 0.14), at=(0, -0.6, 4.0), material="metal")
    b.cylinder(2.2, 2.8, at=(0, -0.6, 8.3), material="metal", segments=20, radius_top=2.2)
    b.cylinder(2.2, 1.2, at=(0, -0.6, 7.1), material="metal", segments=20, radius_top=1.9)
    b.cylinder(2.3, 1.6, at=(0, -0.6, 11.1), material="metal", segments=20, radius_top=0.25)
    b.box((4.5, 4.5, 1.2), at=(0, -0.6, 9.6), material="paint_blue")
    b.box((3.6, 2.4, 2.8), at=(0, 2.4, 0), material="brick", bevel=0.08)
    b.box((1.0, 0.14, 2.1), at=(0, 2.4 - 1.2 - 0.02, 0), material="wood")
    b.gable_roof(3.6, 2.4, 2.8, 0.8, at=(0, 2.4, 0), material="roof_slate", overhang=0.2)
    return b.finish(smooth_angle=35)


def asset_svc_clinic():
    b = Builder("svc_clinic")
    w, d, h = 6.4, 5.2, 7.4
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w, d, h), at=(0, 0, 0), material="plaster", bevel=0.14, segments=2)
    b.box((w + 0.24, d + 0.24, 0.2), at=(0, 0, 3.5), material="concrete")
    b.box((w + 0.30, d + 0.30, 0.3), at=(0, 0, h - 0.15), material="concrete", bevel=0.04)
    for z in (2.0, 5.6):
        b.window_row(4, -d * 0.5 - 0.02, z, 1.05, 1.35, 5.2)
        b.window_row(4, d * 0.5 + 0.02, z, 1.05, 1.35, 5.2)
    b.box((2.4, 0.18, 2.6), at=(0, -d * 0.5 - 0.05, 0), material="window")
    b.box((3.6, 1.6, 0.22), at=(0, -d * 0.5 - 0.8, 2.9), material="metal", bevel=0.04)
    # red cross
    b.box((1.8, 0.14, 0.55), at=(0, -d * 0.5 - 0.10, 5.5), material="emissive_red")
    b.box((0.55, 0.14, 1.8), at=(0, -d * 0.5 - 0.10, 4.9), material="emissive_red")
    # ambulance
    b.box((2.2, 1.3, 1.1), at=(2.1, -2.6, 0.28), material="paint", bevel=0.08)
    b.box((1.0, 1.24, 0.7), at=(1.3, -2.6, 1.10), material="glass_dark", bevel=0.05)
    b.box((2.0, 0.10, 0.30), at=(2.1, -3.26, 0.75), material="paint_red")
    b.box((0.5, 0.5, 0.16), at=(2.4, -2.6, 1.38), material="emissive_red")
    for sx in (1.2, 3.0):
        b.cylinder(0.30, 0.26, at=(sx, -3.25, 0.30), material="road", segments=10)
        b.cylinder(0.30, 0.26, at=(sx, -1.95, 0.30), material="road", segments=10)
    return b.finish(smooth_angle=40)


def asset_svc_school():
    b = Builder("svc_school")
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="grass")
    # L-shaped brick block
    b.box((6.6, 2.6, 4.2), at=(0, 1.9, 0), material="brick", bevel=0.10)
    b.gable_roof(6.6, 2.6, 4.2, 1.4, at=(0, 1.9, 0), material="roof_slate", overhang=0.3)
    b.box((2.6, 3.2, 4.2), at=(-2.0, -1.0, 0), material="brick", bevel=0.10)
    b.gable_roof(2.6, 3.2, 4.2, 1.2, at=(-2.0, -1.0, 0), material="roof_slate",
                 overhang=0.3, along_x=False)
    b.window_row(5, 1.9 - 1.3 - 0.02, 2.4, 0.95, 1.25, 5.4)
    b.window_row(3, -1.0 - 1.6 - 0.02, 2.4, 0.95, 1.25, 2.0)
    b.box((1.6, 0.14, 2.4), at=(-2.0, -2.62, 0), material="window")
    # flagpole + playground
    b.cylinder(0.06, 6.0, at=(2.2, -1.6, 0), material="metal", segments=8)
    b.box((0.9, 0.04, 0.6), at=(2.68, -1.6, 5.2), material="paint_red")
    for i in range(3):
        b.box((0.7, 0.7, 0.5 + i * 0.35), at=(0.4 + i * 1.1, -2.6, 0),
              material="paint_yellow", bevel=0.05)
    return b.finish(smooth_angle=40)


def asset_svc_police():
    b = Builder("svc_police")
    w, d, h = 6.0, 4.8, 6.8
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="concrete")
    b.box((w, d, h), at=(0, 0.6, 0), material="concrete", bevel=0.16, segments=2)
    b.box((w + 0.3, d + 0.3, 0.3), at=(0, 0.6, h - 0.15), material="concrete", bevel=0.04)
    for z in (2.0, 5.2):
        b.window_row(4, 0.6 - d * 0.5 - 0.02, z, 0.95, 1.2, 4.8)
    b.box((2.4, 0.2, 2.6), at=(0, 0.6 - d * 0.5 - 0.05, 0), material="window")
    b.box((3.0, 0.14, 0.6), at=(0, 0.6 - d * 0.5 - 0.12, 3.2), material="paint_blue")
    b.cylinder(0.08, 5.0, at=(-2.2, 2.4, h), material="metal", segments=8, radius_top=0.04)
    # two patrol cars
    for sx in (-1.9, 0.5):
        b.box((2.1, 1.2, 0.95), at=(sx, -2.7, 0.26), material="paint", bevel=0.09)
        b.box((1.1, 1.16, 0.6), at=(sx - 0.1, -2.7, 0.95), material="glass_dark", bevel=0.05)
        b.box((2.12, 0.1, 0.34), at=(sx, -3.31, 0.45), material="paint_blue")
        b.box((0.6, 0.36, 0.14), at=(sx, -2.7, 1.55), material="emissive_red")
        for dx in (-0.7, 0.7):
            b.cylinder(0.28, 0.24, at=(sx + dx, -3.30, 0.28), material="road", segments=10)
            b.cylinder(0.28, 0.24, at=(sx + dx, -2.10, 0.28), material="road", segments=10)
    return b.finish(smooth_angle=40)


def asset_svc_park():
    b = Builder("svc_park")
    b.box((7.2, 7.2, 0.06), at=(0, 0, 0), material="grass")
    # crossing paths
    b.box((7.2, 1.1, 0.05), at=(0, 0, 0.05), material="concrete")
    b.box((1.1, 7.2, 0.05), at=(0, 0, 0.05), material="concrete")
    # fountain
    b.cylinder(1.45, 0.42, at=(0, 0, 0.06), material="concrete", segments=20)
    b.cylinder(1.25, 0.34, at=(0, 0, 0.10), material="window", segments=20)
    b.cylinder(0.30, 1.15, at=(0, 0, 0.44), material="concrete", segments=12, radius_top=0.22)
    b.sphere(0.42, at=(0, 0, 1.85), material="window", subdiv=2)
    # benches, lamp, trees
    for (x, y, rot) in ((-2.1, 1.2, 0), (2.1, -1.2, 0)):
        b.box((1.5, 0.5, 0.12), at=(x, y, 0.44), material="wood", bevel=0.03)
        b.box((1.5, 0.1, 0.5), at=(x, y + 0.22, 0.56), material="wood", bevel=0.03)
        for dx in (-0.6, 0.6):
            b.box((0.12, 0.5, 0.44), at=(x + dx, y, 0.06), material="metal")
    b.cylinder(0.07, 3.4, at=(1.9, 1.9, 0.06), material="metal", segments=8)
    b.sphere(0.26, at=(1.9, 1.9, 3.55), material="emissive", subdiv=1)
    for (x, y) in ((-2.6, -2.6), (2.6, 2.6), (-2.6, 2.6)):
        b.cylinder(0.16, 2.0, at=(x, y, 0.06), material="bark", segments=8, radius_top=0.12)
        b.sphere(1.15, at=(x, y, 3.0), material="foliage", subdiv=2)
        b.sphere(0.8, at=(x + 0.5, y - 0.4, 3.6), material="foliage_dark", subdiv=2)
    return b.finish(smooth_angle=40)


def asset_svc_transit():
    b = Builder("svc_transit")
    b.box((7.0, 7.0, 0.06), at=(0, 0, 0), material="road")
    # garage hall with roller doors
    b.box((6.8, 3.6, 4.6), at=(0, 1.6, 0), material="metal", bevel=0.08)
    for i in range(3):
        b.box((1.6, 0.16, 3.6), at=(-2.2 + i * 2.2, 1.6 - 1.8 - 0.03, 0), material="paint")
    # office corner
    b.box((3.0, 2.6, 3.2), at=(2.0, -2.0, 0), material="plaster", bevel=0.08)
    b.window_row(2, -2.0 - 1.3 - 0.02, 2.2, 1.0, 1.2, 2.0)
    b.box((1.0, 0.14, 2.2), at=(2.0, -3.32, 0), material="window")
    b.box((2.8, 0.12, 0.6), at=(2.0, -3.36, 3.3), material="emissive")
    # a bus parked outside
    b.box((4.6, 1.9, 2.0), at=(-1.6, -2.3, 0.32), material="paint_yellow", bevel=0.14)
    b.box((4.3, 1.94, 0.85), at=(-1.6, -2.3, 1.35), material="glass_dark", bevel=0.06)
    for dx in (-1.5, 1.4):
        b.cylinder(0.34, 0.28, at=(-1.6 + dx, -3.3, 0.34), material="road", segments=12)
        b.cylinder(0.34, 0.28, at=(-1.6 + dx, -1.3, 0.34), material="road", segments=12)
    return b.finish(smooth_angle=40)


# --------------------------------------------------------------------------
# Nature and vehicles
# --------------------------------------------------------------------------
def asset_tree_broadleaf():
    b = Builder("tree_broadleaf")
    b.cylinder(0.30, 2.8, at=(0, 0, 0), material="bark", segments=8, radius_top=0.20)
    b.sphere(1.65, at=(0, 0, 3.8), material="foliage", subdiv=2)
    b.sphere(1.10, at=(0.95, 0.35, 3.1), material="foliage_dark", subdiv=2)
    b.sphere(1.00, at=(-0.85, -0.55, 3.4), material="foliage", subdiv=2)
    b.sphere(0.85, at=(0.15, -0.9, 4.5), material="foliage_dark", subdiv=2)
    return b.finish(smooth_angle=60)


def asset_tree_conifer():
    b = Builder("tree_conifer")
    b.cylinder(0.24, 1.4, at=(0, 0, 0), material="bark", segments=8, radius_top=0.18)
    b.cylinder(1.55, 2.2, at=(0, 0, 1.0), material="foliage_dark", segments=12, radius_top=0.05)
    b.cylinder(1.20, 2.0, at=(0, 0, 2.3), material="foliage_dark", segments=12, radius_top=0.05)
    b.cylinder(0.85, 1.9, at=(0, 0, 3.5), material="foliage", segments=12, radius_top=0.02)
    return b.finish(smooth_angle=50)


def asset_tree_birch():
    b = Builder("tree_birch")
    b.cylinder(0.17, 3.6, at=(0, 0, 0), material="paint", segments=8, radius_top=0.11)
    b.sphere(1.15, at=(0, 0, 4.3), material="foliage", subdiv=2)
    b.sphere(0.80, at=(0.5, 0.3, 4.9), material="foliage", subdiv=2)
    return b.finish(smooth_angle=60)


def asset_car_sedan():
    b = Builder("car_sedan")
    # body: lower shell + cabin, both bevelled
    b.box((4.1, 1.8, 0.75), at=(0, 0, 0.34), material="paint", bevel=0.16, segments=2)
    b.box((2.3, 1.62, 0.62), at=(-0.15, 0, 1.05), material="glass_dark", bevel=0.14, segments=2)
    b.box((4.15, 1.84, 0.10), at=(0, 0, 0.60), material="metal")
    for dx in (-1.35, 1.35):
        for dy in (-0.86, 0.86):
            b.cylinder(0.34, 0.26, at=(dx, dy, 0.34), material="road", segments=12)
            b.cylinder(0.17, 0.28, at=(dx, dy, 0.34), material="metal", segments=10)
    b.box((0.16, 0.44, 0.22), at=(2.02, -0.55, 0.62), material="emissive")
    b.box((0.16, 0.44, 0.22), at=(2.02, 0.55, 0.62), material="emissive")
    b.box((0.14, 0.40, 0.20), at=(-2.03, -0.55, 0.66), material="emissive_red")
    b.box((0.14, 0.40, 0.20), at=(-2.03, 0.55, 0.66), material="emissive_red")
    return b.finish(smooth_angle=45)


def asset_car_van():
    b = Builder("car_van")
    b.box((4.4, 1.95, 1.65), at=(0, 0, 0.34), material="paint", bevel=0.18, segments=2)
    b.box((1.2, 1.86, 0.72), at=(1.35, 0, 1.15), material="glass_dark", bevel=0.10)
    for dx in (-1.45, 1.45):
        for dy in (-0.92, 0.92):
            b.cylinder(0.36, 0.28, at=(dx, dy, 0.36), material="road", segments=12)
    b.box((0.16, 0.42, 0.22), at=(2.18, -0.6, 0.62), material="emissive")
    b.box((0.16, 0.42, 0.22), at=(2.18, 0.6, 0.62), material="emissive")
    b.box((0.14, 0.40, 0.20), at=(-2.2, -0.6, 0.7), material="emissive_red")
    b.box((0.14, 0.40, 0.20), at=(-2.2, 0.6, 0.7), material="emissive_red")
    return b.finish(smooth_angle=45)


def asset_boat():
    b = Builder("boat")
    hull = [(-3.0, -1.15), (1.4, -1.15), (3.6, 0.0), (1.4, 1.15), (-3.0, 1.15)]
    b.prism(hull, 1.15, at=(0, 0, -0.55), material="paint")
    b.prism([(x * 0.97, y * 0.97) for x, y in hull], 0.14, at=(0, 0, 0.60), material="wood")
    b.box((2.1, 1.7, 1.25), at=(-0.7, 0, 0.72), material="plaster", bevel=0.10)
    b.box((1.75, 1.76, 0.55), at=(-0.7, 0, 1.15), material="glass_dark")
    b.cylinder(0.07, 3.0, at=(0.9, 0, 0.72), material="metal", segments=8)
    b.sphere(0.14, at=(0.9, 0, 3.8), material="emissive", subdiv=1)
    return b.finish(smooth_angle=45)


def asset_street_lamp():
    b = Builder("street_lamp")
    b.cylinder(0.15, 4.6, at=(0, 0, 0), material="metal", segments=10, radius_top=0.10)
    # arm curving over the road (-Y)
    b.box((0.12, 0.55, 0.12), at=(0, -0.28, 4.55), material="metal")
    b.box((0.12, 0.12, 0.42), at=(0, -0.52, 4.60), material="metal")
    b.box((0.12, 0.95, 0.12), at=(0, -1.0, 4.92), material="metal")
    b.box((0.62, 0.42, 0.26), at=(0, -1.45, 4.72), material="metal", bevel=0.05)
    b.box((0.48, 0.30, 0.10), at=(0, -1.45, 4.66), material="emissive")
    return b.finish(smooth_angle=40)


def asset_bus_stop():
    b = Builder("bus_stop")
    b.box((2.6, 1.2, 0.08), at=(0, 0, 0), material="concrete")
    for dx in (-1.15, 1.15):
        b.cylinder(0.06, 2.4, at=(dx, 0.45, 0.08), material="metal", segments=8)
    b.box((2.6, 1.3, 0.10), at=(0, 0.15, 2.45), material="metal", bevel=0.03)
    b.box((2.5, 0.06, 1.5), at=(0, 0.55, 0.75), material="window")
    b.box((1.5, 0.5, 0.10), at=(0, 0.3, 0.5), material="wood")
    b.box((0.5, 0.09, 0.5), at=(1.0, -0.1, 1.7), material="emissive")
    return b.finish(smooth_angle=40)


ASSETS = [
    asset_res_house, asset_res_townhouse, asset_res_apartment,
    asset_com_shop, asset_com_office, asset_com_tower,
    asset_ind_warehouse, asset_ind_factory, asset_ind_plant,
    asset_svc_power, asset_svc_water, asset_svc_clinic, asset_svc_school,
    asset_svc_police, asset_svc_park, asset_svc_transit,
    asset_tree_broadleaf, asset_tree_conifer, asset_tree_birch,
    asset_car_sedan, asset_car_van, asset_boat,
    asset_street_lamp, asset_bus_stop,
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.normpath(os.path.join(here, "..", "models"))
    os.makedirs(out_dir, exist_ok=True)

    clear_scene()
    ensure_materials()

    total_tris = 0
    for factory in ASSETS:
        obj = factory()
        # Lay the library out in a grid so the .blend is browsable by hand.
        i = ASSETS.index(factory)
        obj.location = Vector(((i % 6) * 12.0, (i // 6) * 12.0, 0.0))
        tris = sum(len(p.vertices) - 2 for p in obj.data.polygons)
        total_tris += tris
        print("ASSET %-18s %5d tris  %2d materials" % (obj.name, tris, len(obj.data.materials)))

    blend_path = os.path.join(out_dir, "city_assets.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)

    # Export with the objects at the origin: the game positions them itself.
    for obj in bpy.data.objects:
        obj.location = Vector((0.0, 0.0, 0.0))
    glb_path = os.path.join(out_dir, "city_assets.glb")
    bpy.ops.export_scene.gltf(
        filepath=glb_path,
        export_format="GLB",
        export_apply=True,          # bake modifiers (edge split)
        export_yup=True,            # +Z up (Blender) -> +Y up (world)
        export_materials="EXPORT",
        export_normals=True,
        export_texcoords=False,
        export_skins=False,
        export_animations=False,
        export_cameras=False,
        export_lights=False,
        use_visible=False,
    )
    print("WROTE %s (%d assets, %d tris)" % (glb_path, len(ASSETS), total_tris))
    print("WROTE %s" % blend_path)


if __name__ == "__main__":
    main()
