/*
 * CityModels.cpp - CitySlice procedural meshes: primitives, terrain, water grid,
 * roads and bridges, lots, buildings, service buildings, trees, vehicles, boats,
 * the sky dome, tool previews and coverage overlays.
 *
 * Everything is flat-shaded vertex-colour geometry (see CityRenderer.h).
 * Building shells are bevelled prisms rather than boxes, with cornices, string
 * courses and plinths, so silhouettes read as architecture instead of cubes.
 */
#include "CityRenderer.h"

#include "Camera.h"
#include "City.h"
#include "CityRendererInternal.h"

#include <algorithm>
#include <Foundation/Profiling/Profiling.h>
#include <cmath>

namespace
{
  /// Extruded polygon: vertical walls with outward edge normals, flat top cap.
  struct Ring
  {
    Vec3 p[16];
    int n = 0;
  };

  Ring Octagon(float x0, float z0, float x1, float z1, float b)
  {
    Ring r;
    r.n = 8;
    b = std::min(b, std::min(x1 - x0, z1 - z0) * 0.49f);
    r.p[0] = { x0 + b, 0, z0 }; r.p[1] = { x1 - b, 0, z0 }; r.p[2] = { x1, 0, z0 + b }; r.p[3] = { x1, 0, z1 - b };
    r.p[4] = { x1 - b, 0, z1 }; r.p[5] = { x0 + b, 0, z1 }; r.p[6] = { x0, 0, z1 - b }; r.p[7] = { x0, 0, z0 + b };
    return r;
  }

  Vec3 Centroid(const Ring& r)
  {
    Vec3 c;
    for (int i = 0; i < r.n; ++i)
      c += r.p[i];
    return c * (1.0f / std::max(1, r.n));
  }
} // namespace

// ============================================================================
// Authored models
// ============================================================================

void CityRenderer::AddModel(Layer& l, const CityAssets::Model& model, Vec3 origin,
  Vec3 forward, float scale, float rise)
{
  // The models are authored facing +Z; rotate that onto `forward` about Y.
  const Vec3 f = Vec3 { forward.x, 0.0f, forward.z }.Normalized();
  const float cs = f.z, sn = f.x; // yaw taking (0,0,1) to f
  const uint32_t base = static_cast<uint32_t>(l.vertices.size());
  l.vertices.reserve(l.vertices.size() + model.vertices.size());
  for (const CityAssets::Vertex& src : model.vertices)
  {
    Vertex v;
    const float px = src.px * scale, py = src.py * scale * rise, pz = src.pz * scale;
    v.px = origin.x + px * cs + pz * sn;
    v.py = origin.y + py;
    v.pz = origin.z - px * sn + pz * cs;
    v.nx = src.nx * cs + src.nz * sn;
    v.ny = src.ny;
    v.nz = -src.nx * sn + src.nz * cs;
    v.rgba = src.rgba;
    l.vertices.push_back(v);
  }
  l.indices.reserve(l.indices.size() + model.indices.size());
  for (uint32_t i : model.indices)
    l.indices.push_back(base + i);
}

const CityAssets::Model* CityRenderer::BuildingModel(int type, int level) const
{
  if (!m_assets.IsLoaded() || level < 1)
    return nullptr;
  static const char* kRes[3] = { "res_house", "res_townhouse", "res_apartment" };
  static const char* kCom[3] = { "com_shop", "com_office", "com_tower" };
  static const char* kInd[3] = { "ind_warehouse", "ind_factory", "ind_plant" };
  const int idx = std::min(level, 3) - 1;
  switch (type)
  {
    case City::Res: return m_assets.Find(kRes[idx]);
    case City::Com: return m_assets.Find(kCom[idx]);
    case City::Ind: return m_assets.Find(kInd[idx]);
    default: return nullptr;
  }
}

// ============================================================================
// Primitives
// ============================================================================

void CityRenderer::AddFace(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, uint32_t color)
{
  const uint32_t base = static_cast<uint32_t>(l.vertices.size());
  for (const Vec3& p : { a, b, c, d })
    l.vertices.push_back({ p.x, p.y, p.z, n.x, n.y, n.z, color });
  const uint32_t idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
  l.indices.insert(l.indices.end(), idx, idx + 6);
}

void CityRenderer::AddFaceN(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 na, Vec3 nb, Vec3 nc,
  Vec3 nd, uint32_t color)
{
  const uint32_t base = static_cast<uint32_t>(l.vertices.size());
  const Vec3 p[4] = { a, b, c, d };
  const Vec3 n[4] = { na, nb, nc, nd };
  for (int i = 0; i < 4; ++i)
    l.vertices.push_back({ p[i].x, p[i].y, p[i].z, n[i].x, n[i].y, n[i].z, color });
  const uint32_t idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
  l.indices.insert(l.indices.end(), idx, idx + 6);
}

void CityRenderer::AddTri(Layer& l, Vec3 a, Vec3 b, Vec3 c, uint32_t color)
{
  Vec3 n = (b - a).Cross(c - a).Normalized();
  const uint32_t base = static_cast<uint32_t>(l.vertices.size());
  for (const Vec3& p : { a, b, c })
    l.vertices.push_back({ p.x, p.y, p.z, n.x, n.y, n.z, color });
  const uint32_t idx[3] = { base, base + 1, base + 2 };
  l.indices.insert(l.indices.end(), idx, idx + 3);
}

/// Quad whose corners go (x0,z0) -> (x1,z0) -> (x1,z1) -> (x0,z1); normal from the
/// actual corner heights so slopes shade correctly.
void CityRenderer::AddGroundQuad(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t color)
{
  Vec3 n = (d - a).Cross(b - a).Normalized();
  if (n.y < 0.0f)
    n = n * -1.0f;
  AddFace(l, a, b, c, d, n, color);
}

void CityRenderer::AddBox(Layer& l, Vec3 mn, Vec3 mx, uint32_t side, uint32_t top)
{
  const Vec3 p000 { mn.x, mn.y, mn.z }, p100 { mx.x, mn.y, mn.z };
  const Vec3 p010 { mn.x, mx.y, mn.z }, p110 { mx.x, mx.y, mn.z };
  const Vec3 p001 { mn.x, mn.y, mx.z }, p101 { mx.x, mn.y, mx.z };
  const Vec3 p011 { mn.x, mx.y, mx.z }, p111 { mx.x, mx.y, mx.z };
  AddFace(l, p010, p110, p111, p011, { 0, 1, 0 }, top);              // top
  AddFace(l, p001, p101, p111, p011, { 0, 0, 1 }, side);             // +z
  AddFace(l, p100, p000, p010, p110, { 0, 0, -1 }, Shade(side, -12)); // -z
  AddFace(l, p101, p100, p110, p111, { 1, 0, 0 }, Shade(side, -6));   // +x
  AddFace(l, p000, p001, p011, p010, { -1, 0, 0 }, Shade(side, -18)); // -x
  AddFace(l, p001, p000, p100, p101, { 0, -1, 0 }, Shade(side, -30)); // bottom (visible under overhangs)
}

/// Box with bevelled vertical and top edges: an octagonal prism, a lofted
/// chamfer band and an inset top cap. The building shell primitive.
void CityRenderer::AddChamferBox(Layer& l, Vec3 mn, Vec3 mx, float bevel, uint32_t side, uint32_t top)
{
  const float b = std::min(bevel, std::min(std::min(mx.x - mn.x, mx.z - mn.z) * 0.3f, (mx.y - mn.y) * 0.45f));
  const Ring outer = Octagon(mn.x, mn.z, mx.x, mx.z, b);
  const Ring inner = Octagon(mn.x + b, mn.z + b, mx.x - b, mx.z - b, b);
  const float y0 = mn.y, y1 = mx.y - b, y2 = mx.y;
  const Vec3 c = Centroid(outer);
  for (int i = 0; i < 8; ++i)
  {
    const Vec3 a = outer.p[i], d = outer.p[(i + 1) % 8];
    Vec3 n = Vec3 { d.z - a.z, 0.0f, -(d.x - a.x) }.Normalized();
    if (n.Dot((a + d) * 0.5f - c) < 0.0f)
      n = n * -1.0f;
    const int shade = static_cast<int>(-9.0f + 7.0f * n.z - 7.0f * n.x);
    AddFace(l, { a.x, y0, a.z }, { d.x, y0, d.z }, { d.x, y1, d.z }, { a.x, y1, a.z }, n, Shade(side, shade));
    // chamfer band up to the cap
    const Vec3 ia = inner.p[i], id = inner.p[(i + 1) % 8];
    const Vec3 bn = (n + Vec3 { 0, 1, 0 }).Normalized();
    AddFace(l, { a.x, y1, a.z }, { d.x, y1, d.z }, { id.x, y2, id.z }, { ia.x, y2, ia.z }, bn, Shade(side, shade + 6));
  }
  const Vec3 ic = Centroid(inner);
  for (int i = 0; i < 8; ++i)
  {
    const Vec3 a = inner.p[i], d = inner.p[(i + 1) % 8];
    const uint32_t base = static_cast<uint32_t>(l.vertices.size());
    for (const Vec3& p : { Vec3 { ic.x, y2, ic.z }, Vec3 { a.x, y2, a.z }, Vec3 { d.x, y2, d.z } })
      l.vertices.push_back({ p.x, p.y, p.z, 0.0f, 1.0f, 0.0f, top });
    const uint32_t idx[3] = { base, base + 1, base + 2 };
    l.indices.insert(l.indices.end(), idx, idx + 3);
  }
}

void CityRenderer::AddTileFrame(Layer& l, const City& city, int x, int y, float lift,
  uint32_t color, float w)
{
  const float top = city.TileTop(x, y) + lift;
  const float x0 = x * kCell, x1 = x0 + kCell, z0 = y * kCell, z1 = z0 + kCell;
  const Vec3 up { 0, 1, 0 };
  AddFace(l, { x0, top, z0 }, { x1, top, z0 }, { x1, top, z0 + w }, { x0, top, z0 + w }, up, color);
  AddFace(l, { x0, top, z1 - w }, { x1, top, z1 - w }, { x1, top, z1 }, { x0, top, z1 }, up, color);
  AddFace(l, { x0, top, z0 }, { x0 + w, top, z0 }, { x0 + w, top, z1 }, { x0, top, z1 }, up, color);
  AddFace(l, { x1 - w, top, z0 }, { x1, top, z0 }, { x1, top, z1 }, { x1 - w, top, z1 }, up, color);
}

/// Tapered cylinder standing on `c` (base centre). Smooth radial normals.
void CityRenderer::AddCylinder(Layer& l, Vec3 c, float r0, float r1, float h, int sides,
  uint32_t color, bool cap)
{
  const float slope = (r0 - r1) / std::max(h, 0.001f);
  for (int i = 0; i < sides; ++i)
  {
    const float a0 = static_cast<float>(i) / sides * 2.0f * kPi;
    const float a1 = static_cast<float>(i + 1) / sides * 2.0f * kPi;
    const Vec3 d0 { std::cos(a0), 0.0f, std::sin(a0) }, d1 { std::cos(a1), 0.0f, std::sin(a1) };
    const Vec3 n0 = Vec3 { d0.x, slope, d0.z }.Normalized(), n1 = Vec3 { d1.x, slope, d1.z }.Normalized();
    const Vec3 b0 = c + d0 * r0, b1 = c + d1 * r0;
    const Vec3 t0 = c + d0 * r1 + Vec3 { 0, h, 0 }, t1 = c + d1 * r1 + Vec3 { 0, h, 0 };
    AddFaceN(l, b1, b0, t0, t1, n1, n0, n0, n1, color);
  }
  if (cap && r1 > 0.01f)
  {
    const Vec3 top = c + Vec3 { 0, h, 0 };
    for (int i = 0; i < sides; ++i)
    {
      const float a0 = static_cast<float>(i) / sides * 2.0f * kPi;
      const float a1 = static_cast<float>(i + 1) / sides * 2.0f * kPi;
      AddTri(l, top, top + Vec3 { std::cos(a0) * r1, 0, std::sin(a0) * r1 },
        top + Vec3 { std::cos(a1) * r1, 0, std::sin(a1) * r1 }, Shade(color, 10));
    }
  }
}

/// Cylinder centred on `c` along `axis` (wheels, masts, pipes), capped both ends.
void CityRenderer::AddCylinderAxis(Layer& l, Vec3 c, Vec3 axis, float r, float len, int sides, uint32_t color)
{
  const Vec3 a = axis.Normalized();
  const Vec3 helper = std::fabs(a.y) < 0.9f ? Vec3 { 0, 1, 0 } : Vec3 { 1, 0, 0 };
  const Vec3 u = a.Cross(helper).Normalized(), v = a.Cross(u);
  const Vec3 e0 = c - a * (len * 0.5f), e1 = c + a * (len * 0.5f);
  for (int i = 0; i < sides; ++i)
  {
    const float a0 = static_cast<float>(i) / sides * 2.0f * kPi;
    const float a1 = static_cast<float>(i + 1) / sides * 2.0f * kPi;
    const Vec3 n0 = u * std::cos(a0) + v * std::sin(a0), n1 = u * std::cos(a1) + v * std::sin(a1);
    AddFaceN(l, e0 + n0 * r, e0 + n1 * r, e1 + n1 * r, e1 + n0 * r, n0, n1, n1, n0, color);
    AddTri(l, e0, e0 + n1 * r, e0 + n0 * r, Shade(color, -10));
    AddTri(l, e1, e1 + n0 * r, e1 + n1 * r, Shade(color, -10));
  }
}

/// Low-poly sphere with per-vertex radius jitter.
void CityRenderer::AddSphere(Layer& l, Vec3 c, float r, int segs, int rings, uint32_t color,
  uint32_t seed, float jitter)
{
  auto point = [&](int i, int j, Vec3& p, Vec3& n) {
    const float th = static_cast<float>(i % segs) / segs * 2.0f * kPi;
    const float ph = (static_cast<float>(j) / rings - 0.5f) * kPi;
    n = { std::cos(ph) * std::cos(th), std::sin(ph), std::cos(ph) * std::sin(th) };
    const uint32_t hj = Hash8(i % segs, j, static_cast<int>(seed));
    const float rr = r * (1.0f + (static_cast<float>(hj) / 255.0f - 0.5f) * jitter);
    p = c + n * rr;
  };
  for (int j = 0; j < rings; ++j)
    for (int i = 0; i < segs; ++i)
    {
      Vec3 p00, p10, p01, p11, n00, n10, n01, n11;
      point(i, j, p00, n00);
      point(i + 1, j, p10, n10);
      point(i, j + 1, p01, n01);
      point(i + 1, j + 1, p11, n11);
      const uint32_t col = Shade(color, static_cast<int>(Hash8(i, j, static_cast<int>(seed) + 3) % 18) - 9);
      AddFaceN(l, p00, p01, p11, p10, n00, n01, n11, n10, col);
    }
}

/// Canopy blob: a jittered sphere whose normals point away from the tree's
/// canopy centre `nc`, so overlapping blobs shade as one soft mass.
void CityRenderer::AddCanopy(Layer& l, Vec3 c, Vec3 nc, float r, int segs, int rings, uint32_t color, uint32_t seed)
{
  auto point = [&](int i, int j, Vec3& p, Vec3& n) {
    const float th = static_cast<float>(i % segs) / segs * 2.0f * kPi;
    const float ph = (static_cast<float>(j) / rings - 0.5f) * kPi;
    const Vec3 d { std::cos(ph) * std::cos(th), std::sin(ph), std::cos(ph) * std::sin(th) };
    const uint32_t hj = Hash8(i % segs, j, static_cast<int>(seed));
    const float rr = r * (1.0f + (static_cast<float>(hj) / 255.0f - 0.5f) * 0.22f);
    p = c + d * rr;
    n = ((p - nc).Normalized() * 0.7f + d * 0.3f).Normalized();
  };
  for (int j = 0; j < rings; ++j)
    for (int i = 0; i < segs; ++i)
    {
      Vec3 p00, p10, p01, p11, n00, n10, n01, n11;
      point(i, j, p00, n00);
      point(i + 1, j, p10, n10);
      point(i, j + 1, p01, n01);
      point(i + 1, j + 1, p11, n11);
      const uint32_t col = Shade(color, static_cast<int>(Hash8(i, j, static_cast<int>(seed) + 3) % 14) - 7);
      AddFaceN(l, p00, p01, p11, p10, n00, n01, n11, n10, col);
    }
}

/// Gable (pitched) roof over the rectangle mn.xz..mx.xz at eave height `eave`.
/// Ridge runs along X when `alongX`; the end walls are filled with triangles.
void CityRenderer::AddGable(Layer& l, Vec3 mn, Vec3 mx, float eave, float ridgeH, bool alongX,
  uint32_t wall, uint32_t roof, float over)
{
  const float cx = (mn.x + mx.x) * 0.5f, cz = (mn.z + mx.z) * 0.5f, top = eave + ridgeH;
  if (alongX)
  {
    AddTri(l, { mn.x, eave, mn.z }, { cx, top, mn.z }, { mx.x, eave, mn.z }, Shade(wall, -12));
    AddTri(l, { mx.x, eave, mx.z }, { cx, top, mx.z }, { mn.x, eave, mx.z }, wall);
    const float x0 = mn.x - over, x1 = mx.x + over, z0 = mn.z - over, z1 = mx.z + over;
    const float e = eave - over * ridgeH / ((mx.z - mn.z) * 0.5f);
    const Vec3 nA = Vec3 { 0, (mx.z - mn.z) * 0.5f, -ridgeH }.Normalized();
    const Vec3 nB = Vec3 { 0, (mx.z - mn.z) * 0.5f, ridgeH }.Normalized();
    AddFace(l, { x0, e, z0 }, { x1, e, z0 }, { x1, top, cz }, { x0, top, cz }, nA, roof);
    AddFace(l, { x1, e, z1 }, { x0, e, z1 }, { x0, top, cz }, { x1, top, cz }, nB, roof);
    AddFace(l, { x1, e - 0.18f, z0 }, { x0, e - 0.18f, z0 }, { x0, top - 0.18f, cz }, { x1, top - 0.18f, cz }, nA * -1.0f, Shade(roof, -30));
    AddFace(l, { x0, e - 0.18f, z1 }, { x1, e - 0.18f, z1 }, { x1, top - 0.18f, cz }, { x0, top - 0.18f, cz }, nB * -1.0f, Shade(roof, -30));
  }
  else
  {
    AddTri(l, { mn.x, eave, mx.z }, { mn.x, top, cz }, { mn.x, eave, mn.z }, Shade(wall, -18));
    AddTri(l, { mx.x, eave, mn.z }, { mx.x, top, cz }, { mx.x, eave, mx.z }, Shade(wall, -6));
    const float x0 = mn.x - over, x1 = mx.x + over, z0 = mn.z - over, z1 = mx.z + over;
    const float e = eave - over * ridgeH / ((mx.x - mn.x) * 0.5f);
    const Vec3 nA = Vec3 { -ridgeH, (mx.x - mn.x) * 0.5f, 0 }.Normalized();
    const Vec3 nB = Vec3 { ridgeH, (mx.x - mn.x) * 0.5f, 0 }.Normalized();
    AddFace(l, { x0, e, z1 }, { x0, e, z0 }, { cx, top, z0 }, { cx, top, z1 }, nA, roof);
    AddFace(l, { x1, e, z0 }, { x1, e, z1 }, { cx, top, z1 }, { cx, top, z0 }, nB, roof);
    AddFace(l, { x0, e - 0.18f, z0 }, { x0, e - 0.18f, z1 }, { cx, top - 0.18f, z1 }, { cx, top - 0.18f, z0 }, nA * -1.0f, Shade(roof, -30));
    AddFace(l, { x1, e - 0.18f, z1 }, { x1, e - 0.18f, z0 }, { cx, top - 0.18f, z0 }, { cx, top - 0.18f, z1 }, nB * -1.0f, Shade(roof, -30));
  }
}

/// Hip roof: a pyramid frustum from the eave rectangle up to an inset flat top.
void CityRenderer::AddHip(Layer& l, Vec3 mn, Vec3 mx, float eave, float h, float inset,
  uint32_t roof, float over)
{
  const float x0 = mn.x - over, x1 = mx.x + over, z0 = mn.z - over, z1 = mx.z + over;
  const float tx0 = mn.x + inset, tx1 = mx.x - inset, tz0 = mn.z + inset, tz1 = mx.z - inset;
  const float top = eave + h;
  const Vec3 a { x0, eave, z0 }, b { x1, eave, z0 }, c { x1, eave, z1 }, d { x0, eave, z1 };
  const Vec3 ta { tx0, top, tz0 }, tb { tx1, top, tz0 }, tc { tx1, top, tz1 }, td { tx0, top, tz1 };
  auto slope = [&](Vec3 p, Vec3 q, Vec3 r, Vec3 s) {
    Vec3 n = (q - p).Cross(s - p).Normalized();
    if (n.y < 0.0f)
      n = n * -1.0f;
    AddFace(l, p, q, r, s, n, roof);
  };
  slope(b, a, ta, tb);
  slope(d, c, tc, td);
  slope(c, b, tb, tc);
  slope(a, d, td, ta);
  AddFace(l, ta, tb, tc, td, { 0, 1, 0 }, Shade(roof, -8));
  // eave thickness
  AddFace(l, { x0, eave - 0.2f, z0 }, { x1, eave - 0.2f, z0 }, { x1, eave, z0 }, { x0, eave, z0 }, { 0, 0, -1 }, Shade(roof, -25));
  AddFace(l, { x1, eave - 0.2f, z1 }, { x0, eave - 0.2f, z1 }, { x0, eave, z1 }, { x1, eave, z1 }, { 0, 0, 1 }, Shade(roof, -25));
  AddFace(l, { x1, eave - 0.2f, z0 }, { x1, eave - 0.2f, z1 }, { x1, eave, z1 }, { x1, eave, z0 }, { 1, 0, 0 }, Shade(roof, -25));
  AddFace(l, { x0, eave - 0.2f, z1 }, { x0, eave - 0.2f, z0 }, { x0, eave, z0 }, { x0, eave, z1 }, { -1, 0, 0 }, Shade(roof, -25));
}

/// Window pane proud of a wall: `c` is the pane centre on the wall plane, `n`
/// the outward normal, `r` the wall's rightward direction. A dark frame sits
/// behind the glass and a sill below so the pane reads inset from a distance too.
void CityRenderer::AddWindow(Layer& l, Vec3 c, Vec3 n, Vec3 r, float w, float h, uint32_t glass,
  uint32_t frame)
{
  const Vec3 up { 0, 1, 0 };
  const Vec3 out = n * 0.06f;
  const float fw = w * 0.5f + 0.12f, fh = h * 0.5f + 0.12f;
  AddFace(l, c + out * 0.5f - r * fw - up * fh, c + out * 0.5f + r * fw - up * fh,
    c + out * 0.5f + r * fw + up * fh, c + out * 0.5f - r * fw + up * fh, n, frame);
  AddFace(l, c + out - r * (w * 0.5f) - up * (h * 0.5f), c + out + r * (w * 0.5f) - up * (h * 0.5f),
    c + out + r * (w * 0.5f) + up * (h * 0.5f), c + out - r * (w * 0.5f) + up * (h * 0.5f), n, glass);
  // sill
  const Vec3 s0 = c - up * (fh + 0.02f);
  AddFace(l, s0 - r * (fw + 0.05f), s0 + r * (fw + 0.05f), s0 + r * (fw + 0.05f) + n * 0.16f, s0 - r * (fw + 0.05f) + n * 0.16f,
    up, Col(200, 198, 192, MatConcrete));
}

void CityRenderer::AddSkyDome(Layer& l, Vec3 eye, double day)
{
  // A hemisphere fan around the eye, coloured horizon->zenith with a sun glow
  // baked into the vertices. The shader adds the sun disc and clouds.
  const DayLight L = ComputeDayLight(day);
  constexpr int kSeg = 32;
  static const float kElev[6] = { -0.06f, 0.05f, 0.18f, 0.4f, 0.7f, 1.0f };
  const float radius = 2500.0f;
  uint32_t ring[6][kSeg + 1];
  Vec3 pos[6][kSeg + 1];
  for (int r = 0; r < 6; ++r)
    for (int s = 0; s <= kSeg; ++s)
    {
      const float a = static_cast<float>(s) / kSeg * 2.0f * kPi;
      const float e = kElev[r];
      const float ce = std::sqrt(std::max(0.0f, 1.0f - e * e));
      const Vec3 dir { std::cos(a) * ce, e, std::sin(a) * ce };
      const float t = Smooth01((e + 0.06f) / 0.9f);
      Vec3 c = L.horizon * (1.0f - t) + L.zenith * t;
      const float sd = std::max(0.0f, dir.Dot(L.sunDir));
      c += L.sunColor * (std::pow(sd, 10.0f) * 0.25f + std::pow(sd, 3.0f) * 0.05f) * (1.0f - L.night);
      ring[r][s] = ColF(c, MatSky);
      pos[r][s] = eye + dir * radius;
    }
  for (int r = 0; r < 5; ++r)
    for (int s = 0; s < kSeg; ++s)
    {
      const uint32_t base = static_cast<uint32_t>(l.vertices.size());
      const Vec3 p[4] = { pos[r][s], pos[r][s + 1], pos[r + 1][s + 1], pos[r + 1][s] };
      const uint32_t c[4] = { ring[r][s], ring[r][s + 1], ring[r + 1][s + 1], ring[r + 1][s] };
      for (int i = 0; i < 4; ++i)
      {
        const Vec3 n = (eye - p[i]).Normalized();
        l.vertices.push_back({ p[i].x, p[i].y, p[i].z, n.x, n.y, n.z, c[i] });
      }
      const uint32_t idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
      l.indices.insert(l.indices.end(), idx, idx + 6);
    }
}

// ============================================================================
// Terrain + water
// ============================================================================

void CityRenderer::BuildTerrain(const City& city)
{
  NS_PROFILE_SCOPE("City::BuildTerrain");
  Layer& l = m_terrain;
  l.vertices.clear();
  l.indices.clear();
  l.vertices.reserve(City::N * City::N * 4);
  l.indices.reserve(City::N * City::N * 6);

  // Smooth-shaded: normals and colours are per CORNER (central differences of
  // the height field, colour bands blended by height/slope/shore distance), so
  // hills roll instead of showing every tile facet. The material stays per
  // face because the alpha byte must not interpolate between ids.
  auto H = [&](int cx, int cy) {
    return city.CornerHeight(std::clamp(cx, 0, City::N), std::clamp(cy, 0, City::N));
  };
  auto cornerNormal = [&](int cx, int cy) {
    return Vec3 { H(cx - 1, cy) - H(cx + 1, cy), 2.0f * kCell, H(cx, cy - 1) - H(cx, cy + 1) }.Normalized();
  };
  auto cornerColor = [&](int cx, int cy) {
    const float h = H(cx, cy);
    const float gx = (H(cx + 1, cy) - H(cx - 1, cy)) / (2.0f * kCell);
    const float gz = (H(cx, cy + 1) - H(cx, cy - 1)) / (2.0f * kCell);
    const float grade = std::sqrt(gx * gx + gz * gz);
    const Vec3 low { 92.0f, 146.0f, 60.0f }, high { 138.0f, 156.0f, 82.0f };
    const Vec3 rock { 122.0f, 116.0f, 106.0f }, sand { 204.0f, 188.0f, 142.0f };
    Vec3 c = low + (high - low) * Smooth01((h - 6.0f) / 7.0f);
    c = c + (rock - c) * Smooth01((grade - 0.28f) / 0.22f);
    c = c + (sand - c) * Smooth01((City::kWaterLevel + 1.3f - h) / 1.3f);
    const float jitter = 1.0f + (static_cast<int>(Hash8(cx, cy, 1) % 15) - 7) * 0.01f;
    return c * (jitter / 255.0f);
  };

  for (int y = 0; y < City::N; ++y)
    for (int x = 0; x < City::N; ++x)
    {
      const float h00 = H(x, y), h10 = H(x + 1, y), h01 = H(x, y + 1), h11 = H(x + 1, y + 1);
      const float avg = (h00 + h10 + h01 + h11) * 0.25f;
      const float slope = std::max(std::max(h00, h10), std::max(h01, h11))
        - std::min(std::min(h00, h10), std::min(h01, h11));
      const uint32_t mat = (avg < City::kWaterLevel + 0.7f || slope > 2.6f) ? MatPlain : MatGrass;
      const float x0 = x * kCell, x1 = x0 + kCell, z0 = y * kCell, z1 = z0 + kCell;
      const Vec3 p[4] = { { x0, h00, z0 }, { x1, h10, z0 }, { x1, h11, z1 }, { x0, h01, z1 } };
      const int cx[4] = { x, x + 1, x + 1, x }, cy[4] = { y, y, y + 1, y + 1 };
      const uint32_t base = static_cast<uint32_t>(l.vertices.size());
      for (int k = 0; k < 4; ++k)
      {
        const Vec3 n = cornerNormal(cx[k], cy[k]);
        l.vertices.push_back({ p[k].x, p[k].y, p[k].z, n.x, n.y, n.z, ColF(cornerColor(cx[k], cy[k]), mat) });
      }
      const uint32_t idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
      l.indices.insert(l.indices.end(), idx, idx + 6);
    }
  l.dirtyMask = kAllSlots;
}

void CityRenderer::BuildWater()
{
  // One sheet at sea level, subdivided so the vertex shader can displace it
  // with the simulated wave heights. Terrain above it hides it by depth.
  Layer& l = m_water;
  l.vertices.clear();
  l.indices.clear();
  const float w = City::N * kCell, wl = City::kWaterLevel;
  const int div = 160;
  l.vertices.reserve(static_cast<size_t>(div + 1) * (div + 1));
  for (int j = 0; j <= div; ++j)
    for (int i = 0; i <= div; ++i)
      l.vertices.push_back({ w * i / div, wl, w * j / div, 0.0f, 1.0f, 0.0f, Col(34, 92, 148, MatWater) });
  for (int j = 0; j < div; ++j)
    for (int i = 0; i < div; ++i)
    {
      const uint32_t a = j * (div + 1) + i, b = a + 1, c = a + div + 1, d = c + 1;
      const uint32_t idx[6] = { a, b, d, a, d, c };
      l.indices.insert(l.indices.end(), idx, idx + 6);
    }
  l.dirtyMask = kAllSlots;
}

// ============================================================================
// Roads, bridges, lots, street furniture
// ============================================================================

void CityRenderer::BuildCity(const City& city)
{
  NS_PROFILE_SCOPE("City::BuildCity");
  Layer& l = m_city;
  l.vertices.clear();
  l.indices.clear();

  const uint32_t asphalt = Col(66, 66, 70, MatRoad), kerb = Col(164, 162, 156, MatConcrete);
  const uint32_t paint = Col(220, 214, 190), yellow = Col(230, 190, 70);
  const uint32_t concrete = Col(178, 176, 170, MatConcrete);
  const Vec3 up { 0, 1, 0 };

  for (int y = 0; y < City::N; ++y)
    for (int x = 0; x < City::N; ++x)
    {
      const City::Tile& t = city.At(x, y);
      const float x0 = x * kCell, x1 = x0 + kCell, z0 = y * kCell, z1 = z0 + kCell;
      if (t.type == City::Empty)
        continue; // wild trees live in m_trees

      if (t.type == City::Road)
      {
        // Road surface height per tile fraction: bilinear over the road corner
        // heights (bridge decks lift their corners, so ramps come for free).
        const float r00 = city.RoadCornerHeight(x, y), r10 = city.RoadCornerHeight(x + 1, y);
        const float r01 = city.RoadCornerHeight(x, y + 1), r11 = city.RoadCornerHeight(x + 1, y + 1);
        auto RH = [&](float fx, float fz) {
          const float a = r00 + (r10 - r00) * fx, b = r01 + (r11 - r01) * fx;
          return a + (b - a) * fz + 0.15f;
        };
        auto P = [&](float fx, float fz, float lift) { return Vec3 { x0 + fx * kCell, RH(fx, fz) + lift, z0 + fz * kCell }; };
        const bool bridge = city.IsBridge(x, y);
        const bool avenue = t.road != 0;
        const bool ew = city.IsRoad(x - 1, y) || city.IsRoad(x + 1, y);
        const bool ns = city.IsRoad(x, y - 1) || city.IsRoad(x, y + 1);

        AddGroundQuad(l, P(0, 0, 0), P(1, 0, 0), P(1, 1, 0), P(0, 1, 0), asphalt);

        // Embankment skirts where the road rides above the terrain (ramps, fills).
        const float t00 = city.CornerHeight(x, y), t10 = city.CornerHeight(x + 1, y);
        const float t01 = city.CornerHeight(x, y + 1), t11 = city.CornerHeight(x + 1, y + 1);
        if (!bridge && std::max(std::max(r00 - t00, r10 - t10), std::max(r01 - t01, r11 - t11)) > 0.25f)
        {
          AddFace(l, { x0, t00 - 0.3f, z0 }, { x1, t10 - 0.3f, z0 }, { x1, r10 + 0.1f, z0 }, { x0, r00 + 0.1f, z0 }, { 0, 0, -1 }, concrete);
          AddFace(l, { x1, t11 - 0.3f, z1 }, { x0, t01 - 0.3f, z1 }, { x0, r01 + 0.1f, z1 }, { x1, r11 + 0.1f, z1 }, { 0, 0, 1 }, concrete);
          AddFace(l, { x1, t10 - 0.3f, z0 }, { x1, t11 - 0.3f, z1 }, { x1, r11 + 0.1f, z1 }, { x1, r10 + 0.1f, z0 }, { 1, 0, 0 }, concrete);
          AddFace(l, { x0, t01 - 0.3f, z1 }, { x0, t00 - 0.3f, z0 }, { x0, r00 + 0.1f, z0 }, { x0, r01 + 0.1f, z1 }, { -1, 0, 0 }, concrete);
        }

        const float k = 0.9f / kCell; // kerb width as a tile fraction
        if (bridge)
        {
          // Deck slab, railings on the free sides, piers down to the lake bed.
          const float deck = City::kBridgeDeck;
          AddBox(l, { x0, deck - 0.8f, z0 }, { x1, deck + 0.05f, z1 }, concrete, concrete);
          auto railing = [&](Vec3 a, Vec3 b, Vec3 side) {
            const Vec3 d = (b - a);
            const int posts = 4;
            for (int i = 0; i <= posts; ++i)
            {
              const Vec3 p = a + d * (static_cast<float>(i) / posts);
              AddBox(l, { p.x - 0.08f, deck + 0.15f, p.z - 0.08f }, { p.x + 0.08f, deck + 1.25f, p.z + 0.08f }, Col(120, 124, 130, MatMetal), Col(120, 124, 130, MatMetal));
            }
            const Vec3 off = side * 0.0f;
            const Vec3 lo = a + off, hi = b + off;
            AddBox(l, { std::min(lo.x, hi.x) - 0.06f, deck + 1.15f, std::min(lo.z, hi.z) - 0.06f },
              { std::max(lo.x, hi.x) + 0.06f, deck + 1.3f, std::max(lo.z, hi.z) + 0.06f }, Col(150, 154, 160, MatMetal), Col(150, 154, 160, MatMetal));
          };
          if (!city.IsRoad(x, y - 1)) railing({ x0, 0, z0 + 0.25f }, { x1, 0, z0 + 0.25f }, { 0, 0, -1 });
          if (!city.IsRoad(x, y + 1)) railing({ x0, 0, z1 - 0.25f }, { x1, 0, z1 - 0.25f }, { 0, 0, 1 });
          if (!city.IsRoad(x - 1, y)) railing({ x0 + 0.25f, 0, z0 }, { x0 + 0.25f, 0, z1 }, { -1, 0, 0 });
          if (!city.IsRoad(x + 1, y)) railing({ x1 - 0.25f, 0, z0 }, { x1 - 0.25f, 0, z1 }, { 1, 0, 0 });
          const float pierX[2] = { ew ? x0 + 4.0f : x0 + 1.6f, ew ? x0 + 4.0f : x1 - 1.6f };
          const float pierZ[2] = { ew ? z0 + 1.6f : z0 + 4.0f, ew ? z1 - 1.6f : z0 + 4.0f };
          for (int i = 0; i < 2; ++i)
          {
            const float ground = city.HeightAt(pierX[i], pierZ[i]) - 1.0f;
            AddCylinder(l, { pierX[i], ground, pierZ[i] }, 0.7f, 0.6f, deck - 0.8f - ground, 10, Shade(concrete, -20), false);
          }
        }
        else
        {
          // Kerbs along the sides that have no continuing road.
          if (!city.IsRoad(x, y - 1)) AddGroundQuad(l, P(0, 0, 0.05f), P(1, 0, 0.05f), P(1, k, 0.05f), P(0, k, 0.05f), kerb);
          if (!city.IsRoad(x, y + 1)) AddGroundQuad(l, P(0, 1 - k, 0.05f), P(1, 1 - k, 0.05f), P(1, 1, 0.05f), P(0, 1, 0.05f), kerb);
          if (!city.IsRoad(x - 1, y)) AddGroundQuad(l, P(0, 0, 0.05f), P(k, 0, 0.05f), P(k, 1, 0.05f), P(0, 1, 0.05f), kerb);
          if (!city.IsRoad(x + 1, y)) AddGroundQuad(l, P(1 - k, 0, 0.05f), P(1, 0, 0.05f), P(1, 1, 0.05f), P(1 - k, 1, 0.05f), kerb);
        }

        // Markings: dashes down straights (double yellow on avenues), zebra
        // stripes on every intersection arm.
        const float lift = 0.03f;
        auto strip = [&](float fx0, float fz0, float fx1, float fz1, uint32_t c) {
          AddGroundQuad(l, P(fx0, fz0, lift), P(fx1, fz0, lift), P(fx1, fz1, lift), P(fx0, fz1, lift), c);
        };
        const float cw = 0.15f / kCell;
        if (ew && !ns)
        {
          if (avenue)
          {
            strip(0, 0.5f - cw * 2.2f, 1, 0.5f - cw * 0.6f, yellow);
            strip(0, 0.5f + cw * 0.6f, 1, 0.5f + cw * 2.2f, yellow);
            strip(0, 0.25f - cw * 0.4f, 1, 0.25f + cw * 0.4f, Shade(paint, -60));
            strip(0, 0.75f - cw * 0.4f, 1, 0.75f + cw * 0.4f, Shade(paint, -60));
          }
          else
            for (int d = 0; d < 3; ++d)
            {
              const float a = (0.8f + d * 2.6f) / kCell, b = a + 1.2f / kCell;
              strip(a, 0.5f - cw, b, 0.5f + cw, paint);
            }
        }
        else if (ns && !ew)
        {
          if (avenue)
          {
            strip(0.5f - cw * 2.2f, 0, 0.5f - cw * 0.6f, 1, yellow);
            strip(0.5f + cw * 0.6f, 0, 0.5f + cw * 2.2f, 1, yellow);
            strip(0.25f - cw * 0.4f, 0, 0.25f + cw * 0.4f, 1, Shade(paint, -60));
            strip(0.75f - cw * 0.4f, 0, 0.75f + cw * 0.4f, 1, Shade(paint, -60));
          }
          else
            for (int d = 0; d < 3; ++d)
            {
              const float a = (0.8f + d * 2.6f) / kCell, b = a + 1.2f / kCell;
              strip(0.5f - cw, a, 0.5f + cw, b, paint);
            }
        }
        else if (ew && ns && !bridge)
        {
          for (int d = 0; d < 4; ++d)
          {
            const float a = (1.4f + d * 1.5f) / kCell, b = a + 0.7f / kCell;
            strip(a, 1.0f / kCell, b, 1.9f / kCell, paint);
            strip(a, 1 - 1.9f / kCell, b, 1 - 1.0f / kCell, paint);
            strip(1.0f / kCell, a, 1.9f / kCell, b, paint);
            strip(1 - 1.9f / kCell, a, 1 - 1.0f / kCell, b, paint);
          }
        }

        // Street furniture on straights: lamps (both kerbs on avenues, alternating
        // on streets) with a real spotlight each, and a kerb tree opposite.
        if (ew != ns && !bridge)
        {
          const bool odd = ((x + y) & 1) != 0;
          const bool aFree = ew ? !city.IsRoad(x, y - 1) : !city.IsRoad(x - 1, y);
          const bool bFree = ew ? !city.IsRoad(x, y + 1) : !city.IsRoad(x + 1, y);
          auto kerbPos = [&](bool sideA, float along, float in, float& px, float& pz) {
            if (ew) { px = x0 + along; pz = sideA ? z0 + in : z1 - in; }
            else    { px = sideA ? x0 + in : x1 - in; pz = z0 + along; }
          };
          auto lamp = [&](bool sideA, float along) {
            float lx, lz;
            kerbPos(sideA, along, 0.45f, lx, lz);
            const float ly = city.RoadHeightAt(lx, lz) + 0.05f;
            const Vec3 toRoad = ew ? Vec3 { 0, 0, sideA ? 1.0f : -1.0f } : Vec3 { sideA ? 1.0f : -1.0f, 0, 0 };
            if (const CityAssets::Model* model = m_assets.IsLoaded() ? m_assets.Find("street_lamp") : nullptr)
            {
              // Authored facing +Z with the arm reaching -Z, so aim it away from the road.
              AddModel(l, *model, { lx, ly, lz }, toRoad * -1.0f);
              AddSpot({ lx + toRoad.x * 1.45f, ly + 4.7f, lz + toRoad.z * 1.45f },
                { toRoad.x * 0.15f, -1.0f, toRoad.z * 0.15f }, { 1.0f, 0.86f, 0.62f },
                55.0f, 17.0f, 38.0f, 60.0f);
              return;
            }
            const uint32_t pole = Col(70, 74, 80, MatMetal);
            AddCylinder(l, { lx, ly, lz }, 0.14f, 0.09f, 4.6f, 8, pole, false);
            const Vec3 in = ew ? Vec3 { 0, 0, sideA ? 1.0f : -1.0f } : Vec3 { sideA ? 1.0f : -1.0f, 0, 0 };
            // curved-looking arm: two segments rising then reaching over the lane
            const Vec3 top { lx, ly + 4.6f, lz };
            const Vec3 mid = top + in * 0.5f + Vec3 { 0, 0.35f, 0 };
            const Vec3 armEnd = top + in * 1.5f + Vec3 { 0, 0.3f, 0 };
            AddCylinderAxis(l, (top + mid) * 0.5f, mid - top, 0.06f, (mid - top).Length(), 6, pole);
            AddCylinderAxis(l, (mid + armEnd) * 0.5f, armEnd - mid, 0.06f, (armEnd - mid).Length(), 6, pole);
            const float hw = ew ? 0.55f : 0.3f, hd = ew ? 0.3f : 0.55f;
            AddChamferBox(l, { armEnd.x - hw, armEnd.y - 0.25f, armEnd.z - hd }, { armEnd.x + hw, armEnd.y + 0.02f, armEnd.z + hd }, 0.08f,
              Col(74, 76, 82, MatMetal), Col(100, 102, 108, MatMetal));
            AddBox(l, { armEnd.x - hw + 0.08f, armEnd.y - 0.3f, armEnd.z - hd + 0.08f },
              { armEnd.x + hw - 0.08f, armEnd.y - 0.22f, armEnd.z + hd - 0.08f },
              Col(255, 228, 170, MatEmissive), Col(255, 228, 170, MatEmissive));
            AddSpot({ armEnd.x, armEnd.y - 0.35f, armEnd.z }, { in.x * 0.15f, -1.0f, in.z * 0.15f }, { 1.0f, 0.86f, 0.62f }, 55.0f, 17.0f, 38.0f, 60.0f);
          };
          auto kerbTree = [&](bool sideA, float along) {
            float tx, tz;
            kerbPos(sideA, along, 0.55f, tx, tz);
            const uint32_t seed = Hash8(x, y, sideA ? 61 : 62);
            AddTree(l, tx, city.HeightAt(tx, tz) - 0.05f, tz, 0.5f + (seed % 5) * 0.04f, (seed % 6)); // broadleaf only
          };
          if (avenue)
          {
            if (aFree) lamp(true, 2.0f);
            if (bFree) lamp(false, 6.0f);
          }
          else if (odd)
          {
            if (aFree) lamp(true, 4.0f);
            if (bFree) kerbTree(false, 4.0f);
          }
          else
          {
            if (bFree) lamp(false, 4.0f);
            if (aFree) kerbTree(true, 4.0f);
          }
        }
        continue;
      }

      if (t.type == City::Service)
      {
        // Paved lot with a low wall, the building itself is in the buildings layer.
        const float top = city.TileTop(x, y);
        AddFace(l, { x0 + 0.2f, top + 0.05f, z0 + 0.2f }, { x1 - 0.2f, top + 0.05f, z0 + 0.2f }, { x1 - 0.2f, top + 0.05f, z1 - 0.2f },
          { x0 + 0.2f, top + 0.05f, z1 - 0.2f }, up, Col(150, 148, 142, MatConcrete));
        continue;
      }

      // Zoned lot: tint the ground; vacant lots show the zone colour clearly.
      const uint32_t tint = t.type == City::Res ? Col(118, 190, 96) : t.type == City::Com ? Col(96, 146, 206) : Col(214, 186, 84);
      if (t.level == 0)
      {
        const float h00 = city.CornerHeight(x, y) + 0.06f, h10 = city.CornerHeight(x + 1, y) + 0.06f;
        const float h01 = city.CornerHeight(x, y + 1) + 0.06f, h11 = city.CornerHeight(x + 1, y + 1) + 0.06f;
        AddGroundQuad(l, { x0 + 0.4f, h00, z0 + 0.4f }, { x1 - 0.4f, h10, z0 + 0.4f },
          { x1 - 0.4f, h11, z1 - 0.4f }, { x0 + 0.4f, h01, z1 - 0.4f }, tint);
      }
      else
        AddTileFrame(l, city, x, y, 0.04f, Shade(tint, -30));
    }
  l.dirtyMask = kAllSlots;
}

// ============================================================================
// Trees
// ============================================================================

void CityRenderer::AddTree(Layer& l, float x, float y, float z, float s, uint32_t seed)
{
  const int kind = static_cast<int>(seed % 10);
  if (m_assets.IsLoaded())
  {
    const char* asset = kind < 6 ? "tree_broadleaf" : (kind < 9 ? "tree_conifer" : "tree_birch");
    if (const CityAssets::Model* model = m_assets.Find(asset))
    {
      // Vary the yaw per tree so a grove does not look cloned.
      const float a = (seed % 64) * 0.0982f;
      AddModel(l, *model, { x, y, z }, { std::sin(a), 0.0f, std::cos(a) }, s);
      return;
    }
  }
  const Vec3 base { x, y, z };
  static const uint32_t leafy[4] = { Col(64, 132, 56, MatFoliage), Col(82, 148, 62, MatFoliage),
    Col(56, 118, 66, MatFoliage), Col(104, 150, 58, MatFoliage) };
  if (kind < 6)
  {
    // Broadleaf: tapered trunk, a cluster of canopy blobs sharing one shading centre.
    const uint32_t canopy = (seed % 23 == 0) ? Col(196, 126, 54, MatFoliage) : leafy[(seed / 10) % 4];
    AddCylinder(l, base, 0.30f * s, 0.18f * s, 2.6f * s, 7, Col(92, 64, 40), false);
    const Vec3 cc = base + Vec3 { 0, 3.5f * s, 0 };
    AddCanopy(l, cc, cc, 1.55f * s, 8, 4, canopy, seed);
    const int lobes = 3 + static_cast<int>((seed >> 2) & 1);
    for (int i = 0; i < lobes; ++i)
    {
      const float a = static_cast<float>(i) / lobes * 2.0f * kPi + (seed % 7) * 0.4f;
      const Vec3 c = cc + Vec3 { std::cos(a) * 1.0f * s, -0.4f * s + (i & 1) * 0.5f * s, std::sin(a) * 1.0f * s };
      AddCanopy(l, c, cc, (1.0f + (Hash8(i, static_cast<int>(seed), 3) % 40) * 0.006f) * s, 7, 3, Shade(canopy, (i & 1) ? 8 : -6), seed + 11 * i);
    }
  }
  else if (kind < 9)
  {
    // Conifer: stacked cones on a short trunk.
    const uint32_t needle = ((seed >> 5) & 1) ? Col(40, 98, 54, MatFoliage) : Col(52, 108, 60, MatFoliage);
    AddCylinder(l, base, 0.24f * s, 0.16f * s, 1.4f * s, 7, Col(84, 58, 38), false);
    AddCylinder(l, base + Vec3 { 0, 1.0f * s, 0 }, 1.55f * s, 0.05f, 2.2f * s, 8, needle, false);
    AddCylinder(l, base + Vec3 { 0, 2.3f * s, 0 }, 1.2f * s, 0.05f, 2.0f * s, 8, Shade(needle, 8), false);
    AddCylinder(l, base + Vec3 { 0, 3.5f * s, 0 }, 0.8f * s, 0.02f, 1.9f * s, 8, Shade(needle, 16), false);
  }
  else
  {
    // Birch: pale slender trunk, two light blobs.
    AddCylinder(l, base, 0.17f * s, 0.11f * s, 3.4f * s, 7, Col(224, 220, 208), false);
    const Vec3 cc = base + Vec3 { 0, 4.0f * s, 0 };
    AddCanopy(l, cc, cc, 1.15f * s, 7, 3, Col(150, 186, 72, MatFoliage), seed);
    AddCanopy(l, cc + Vec3 { 0.5f * s, 0.7f * s, -0.3f * s }, cc, 0.8f * s, 6, 3, Col(160, 196, 80, MatFoliage), seed + 5);
  }
}

void CityRenderer::BuildTrees(const City& city)
{
  NS_PROFILE_SCOPE("City::BuildTrees");
  Layer& l = m_trees;
  l.vertices.clear();
  l.indices.clear();
  m_treeRange.assign(City::N * City::N, { 0u, 0u });
  for (int y = 0; y < City::N; ++y)
    for (int x = 0; x < City::N; ++x)
    {
      const City::Tile& t = city.At(x, y);
      if (t.type != City::Empty || city.IsWater(x, y))
        continue;
      // Wild trees scatter over unused, dry, gentle ground (deterministic per
      // tile); planted tiles get a small grove regardless.
      const bool wild = Hash8(x, y, 7) <= 58;
      if (!wild && !t.planted)
        continue;
      const float top = city.TileTop(x, y);
      const float h00 = city.CornerHeight(x, y), h11 = city.CornerHeight(x + 1, y + 1);
      if (!t.planted && (std::fabs(h00 - h11) > 2.4f || top < City::kWaterLevel + 0.8f))
        continue;
      const float x0 = x * kCell, z0 = y * kCell;
      const uint32_t begin = static_cast<uint32_t>(l.vertices.size());
      const int count = t.planted ? 3 : 1 + (Hash8(x, y, 8) & 1);
      for (int k = 0; k < count; ++k)
      {
        const float ox = 1.5f + (Hash8(x, y, 20 + k) / 255.0f) * 5.0f;
        const float oz = 1.5f + (Hash8(x, y, 30 + k) / 255.0f) * 5.0f;
        const float s = 0.8f + (Hash8(x, y, 40 + k) / 255.0f) * 0.7f;
        AddTree(l, x0 + ox, city.HeightAt(x0 + ox, z0 + oz) - 0.1f, z0 + oz, s, Hash8(x, y, 50 + k));
      }
      m_treeRange[y * City::N + x] = { begin, static_cast<uint32_t>(l.vertices.size()) };
    }
  l.dirtyMask = kAllSlots;
}

void CityRenderer::PruneTrees(const City& city)
{
  // A tile that got a road or a zone loses its trees: collapse their vertices
  // onto one point (degenerate triangles draw nothing) instead of rebuilding
  // the whole layer, which is ~300k vertices.
  Layer& l = m_trees;
  bool changed = false;
  for (int i = 0; i < City::N * City::N; ++i)
  {
    auto& r = m_treeRange[i];
    if (r.second == r.first)
      continue;
    if (city.At(i % City::N, i / City::N).type == City::Empty)
      continue;
    const Vertex& first = l.vertices[r.first];
    for (uint32_t v = r.first + 1; v < r.second; ++v)
    {
      l.vertices[v].px = first.px;
      l.vertices[v].py = first.py;
      l.vertices[v].pz = first.pz;
    }
    r = { 0u, 0u };
    changed = true;
  }
  if (changed)
    l.dirtyMask = kAllSlots;
}

// ============================================================================
// Buildings
// ============================================================================

void CityRenderer::BuildBuildings(const City& city, bool excludeRising)
{
  NS_PROFILE_SCOPE("City::BuildBuildings");
  Layer& l = m_buildings;
  l.vertices.clear();
  l.indices.clear();
  for (int y = 0; y < City::N; ++y)
    for (int x = 0; x < City::N; ++x)
    {
      const City::Tile& t = city.At(x, y);
      if (t.type < City::Res || t.level == 0)
        continue;
      if (excludeRising && city.gameSeconds - t.spawnTime < City::kSpawnAnimSeconds)
        continue; // drawn by BuildDynamic while it rises
      if (t.type == City::Service)
        AddServiceBuilding(l, city, x, y, 1.0f);
      else
        AddBuilding(l, city, x, y, 1.0f);
    }
  l.dirtyMask = kAllSlots;
}

void CityRenderer::AddBuilding(Layer& l, const City& city, int x, int y, float rise)
{
  const City::Tile& t = city.At(x, y);
  const float base = city.TileTop(x, y);
  const float cx = (x + 0.5f) * kCell, cz = (y + 0.5f) * kCell;
  const int v = t.variant;
  const size_t firstVertex = l.vertices.size();
  const size_t firstLight = m_spotSink ? m_spotSink->size() : 0;

  // Face the street: forward `f` points at the neighbouring road, `r` is the
  // building's right. Local coordinates (rx, up, fz) map onto world axes.
  Vec3 f { 0, 0, 1 };
  if (city.IsRoad(x, y + 1)) f = { 0, 0, 1 };
  else if (city.IsRoad(x, y - 1)) f = { 0, 0, -1 };
  else if (city.IsRoad(x + 1, y)) f = { 1, 0, 0 };
  else if (city.IsRoad(x - 1, y)) f = { -1, 0, 0 };

  // Authored model when the library has one for this type and level.
  if (const CityAssets::Model* model = BuildingModel(t.type, t.level))
  {
    AddModel(l, *model, { cx, base, cz }, f, 1.0f, rise);
    if (m_spotSink && rise > 0.999f)
    {
      // Shopfronts and tower entrances light their own frontage at night.
      const Vec3 front = Vec3 { cx, base, cz } + f * 3.0f;
      if (t.type == City::Com)
        AddSpot(front + Vec3 { 0, t.level == 1 ? 3.6f : 3.2f, 0 }, f + Vec3 { 0, -0.6f, 0 },
          { 1.0f, 0.92f, 0.75f }, t.level == 1 ? 14.0f : 10.0f, 9.0f, 45.0f, 75.0f);
      else if (t.type == City::Ind)
        AddSpot(front + Vec3 { 0, 5.0f, 0 }, f + Vec3 { 0, -0.8f, 0 },
          { 1.0f, 0.9f, 0.7f }, 10.0f, 9.0f, 45.0f, 75.0f);
    }
    return;
  }
  const Vec3 r = Vec3 { 0, 1, 0 }.Cross(f);
  const Vec3 up { 0, 1, 0 };
  auto local = [&](float lx, float ly, float lz) { return Vec3 { cx, base, cz } + r * lx + up * ly + f * lz; };
  auto lbox = [&](float hw, float hd, float y0, float y1, float ox, float oz, uint32_t side, uint32_t top) {
    const Vec3 a = local(ox - hw, y0, oz - hd), b = local(ox + hw, y1, oz + hd);
    AddBox(l, { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) },
      { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }, side, top);
  };
  auto lshell = [&](float hw, float hd, float y0, float y1, float ox, float oz, float bevel, uint32_t side, uint32_t top) {
    const Vec3 a = local(ox - hw, y0, oz - hd), b = local(ox + hw, y1, oz + hd);
    AddChamferBox(l, { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) },
      { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }, bevel, side, top);
  };
  auto lrect = [&](float hw, float hd, float ox, float oz, Vec3& mn, Vec3& mx) {
    const Vec3 a = local(ox - hw, 0, oz - hd), b = local(ox + hw, 0, oz + hd);
    mn = { std::min(a.x, b.x), base, std::min(a.z, b.z) };
    mx = { std::max(a.x, b.x), base, std::max(a.z, b.z) };
  };
  // Horizontal band around a shell: cornices, string courses, plinths.
  auto band = [&](float hw, float hd, float y0, float y1, float ox, float oz, float proud, uint32_t c) {
    lbox(hw + proud, hd + proud, y0, y1, ox, oz, c, Shade(c, 8));
  };
  const bool alongX = f.z != 0.0f; // ridge parallel to the street
  auto windowRow = [&](float hw, float hd, float yc, int count, float w, float h, uint32_t glass, bool front,
                     float skipCentre = 0.0f) {
    const Vec3 n = front ? f : f * -1.0f;
    const Vec3 right = front ? r : r * -1.0f;
    for (int i = 0; i < count; ++i)
    {
      const float lx = (count == 1) ? 0.0f : -hw + (hw * 2.0f) * (i + 0.5f) / count;
      if (skipCentre > 0.0f && std::fabs(lx) < skipCentre)
        continue;
      AddWindow(l, local(lx, yc, front ? hd : -hd), n, right, w, h, glass, Col(40, 42, 48));
    }
  };
  auto sideWindows = [&](float hw, float hd, float yc, int count, float w, float h, uint32_t glass) {
    for (int sgn = -1; sgn <= 1; sgn += 2)
    {
      const Vec3 n = r * static_cast<float>(sgn);
      const Vec3 right = f * static_cast<float>(-sgn);
      for (int i = 0; i < count; ++i)
      {
        const float lz = -hd + (hd * 2.0f) * (i + 0.5f) / count;
        AddWindow(l, local(sgn * hw, yc, lz), n, right, w, h, glass, Col(40, 42, 48));
      }
    }
  };
  auto door = [&](float lx, float hd, float w, float h, uint32_t col) {
    AddWindow(l, local(lx, h * 0.5f + 0.05f, hd), f, r, w, h, col, Col(60, 50, 44));
  };
  auto signLight = [&](Vec3 pos, Vec3 tint, float intensity) {
    AddSpot(pos + f * 0.3f, f + Vec3 { 0, -0.6f, 0 }, tint, intensity, 9.0f, 45.0f, 75.0f);
  };

  const uint32_t glass = Col(150, 188, 220, MatWindow);
  const uint32_t glassDark = Col(96, 130, 160, MatWindow);
  const uint32_t trim = Col(226, 222, 212, MatConcrete);

  // Lot pad so a building never floats over a slope, plus a pavement strip.
  {
    Vec3 mn, mx;
    lrect(3.6f, 3.6f, 0, 0, mn, mx);
    AddFace(l, { mn.x, base + 0.05f, mn.z }, { mx.x, base + 0.05f, mn.z }, { mx.x, base + 0.05f, mx.z },
      { mn.x, base + 0.05f, mx.z }, up, t.type == City::Res ? Col(110, 150, 84) : Col(138, 136, 130, MatConcrete));
  }

  switch (t.type)
  {
    case City::Res:
    {
      static const uint32_t brick[3] = { Col(172, 98, 78, MatBrick), Col(150, 86, 66, MatBrick), Col(196, 134, 104, MatBrick) };
      static const uint32_t plaster[4] = { Col(228, 216, 190, MatPlaster), Col(238, 232, 214, MatPlaster),
        Col(206, 192, 160, MatPlaster), Col(196, 206, 196, MatPlaster) };
      static const uint32_t roofs[4] = { Col(118, 54, 46, MatRoof), Col(72, 74, 82, MatRoof), Col(124, 88, 60, MatRoof), Col(66, 92, 74, MatRoof) };
      const uint32_t wall = (v & 1) ? brick[(v >> 1) % 3] : plaster[(v >> 1) % 4];
      const uint32_t roof = roofs[(v >> 3) % 4];
      if (t.level == 1)
      {
        // Detached house: bevelled main block, porch, chimney, hedge and path.
        const float hw = 2.3f, hd = 1.9f, eave = 3.0f;
        Vec3 mn, mx;
        lrect(hw, hd, 0, -0.4f, mn, mx);
        band(hw, hd, 0.0f, 0.35f, 0, -0.4f, 0.08f, Shade(wall, -40));
        lshell(hw, hd, 0.3f, eave, 0, -0.4f, 0.14f, wall, Shade(wall, -10));
        AddGable(l, { mn.x, base, mn.z }, { mx.x, base, mx.z }, base + eave, 1.7f, alongX, wall, roof, 0.4f);
        windowRow(hw, hd, 1.7f, 2, 1.0f, 1.1f, glass, true, 0.6f);
        windowRow(hw, hd, 1.7f, 2, 1.0f, 1.1f, glass, false);
        sideWindows(hw, hd, 1.7f, 1, 0.9f, 1.0f, glass);
        door(0.0f, hd - 0.4f, 0.9f, 2.0f, Col(96, 60, 44));
        lbox(1.0f, 0.55f, 2.35f, 2.5f, 0.0f, hd - 0.4f + 0.3f, Col(120, 100, 88), Col(130, 110, 96)); // porch roof
        AddCylinder(l, local(-0.85f, 0.0f, hd - 0.4f + 0.7f), 0.07f, 0.07f, 2.35f, 6, Col(200, 196, 188), false);
        AddCylinder(l, local(0.85f, 0.0f, hd - 0.4f + 0.7f), 0.07f, 0.07f, 2.35f, 6, Col(200, 196, 188), false);
        lbox(0.3f, 0.3f, eave, eave + 2.6f, 1.3f, -1.0f, Col(110, 88, 80, MatBrick), Col(80, 66, 60));
        lbox(0.6f, 0.15f, 0.0f, 0.06f, 0.0f, 2.6f, Col(170, 166, 156, MatConcrete), Col(170, 166, 156, MatConcrete));
        lbox(3.3f, 0.25f, 0.0f, 0.75f, 0.0f, 3.2f, Col(58, 112, 52, MatFoliage), Col(74, 132, 60, MatFoliage));
        lbox(0.25f, 3.0f, 0.0f, 0.75f, -3.3f, 0.2f, Col(58, 112, 52, MatFoliage), Col(74, 132, 60, MatFoliage));
        if (v & 4)
          AddTree(l, cx + r.x * 2.4f + f.x * -2.3f, base, cz + r.z * 2.4f + f.z * -2.3f, 0.55f, static_cast<uint32_t>(v) * 7u + 1u);
      }
      else if (t.level == 2)
      {
        // Two-storey townhouse: bevelled shell, string course, hip roof, balcony.
        const float hw = 2.9f, hd = 2.5f, eave = 6.4f;
        Vec3 mn, mx;
        lrect(hw, hd, 0, -0.2f, mn, mx);
        band(hw, hd, 0.0f, 0.4f, 0, -0.2f, 0.1f, Shade(wall, -45));
        lshell(hw, hd, 0.35f, eave, 0, -0.2f, 0.18f, wall, Shade(wall, -10));
        band(hw, hd, 3.15f, 3.3f, 0, -0.2f, 0.12f, trim);
        band(hw, hd, eave - 0.3f, eave, 0, -0.2f, 0.2f, trim);
        AddHip(l, { mn.x, base, mn.z }, { mx.x, base, mx.z }, base + eave, 1.6f, 1.4f, roof, 0.45f);
        for (int fl = 0; fl < 2; ++fl)
        {
          const float yc = 1.7f + fl * 3.1f;
          windowRow(hw, hd, yc, 3, 1.0f, 1.3f, glass, true, fl == 0 ? 0.7f : 0.0f);
          windowRow(hw, hd, yc, 3, 1.0f, 1.3f, glass, false);
          sideWindows(hw, hd, yc, 2, 0.9f, 1.2f, glass);
        }
        door(0.0f, hd - 0.2f, 1.0f, 2.2f, Col(80, 56, 44));
        lbox(1.6f, 0.45f, 3.05f, 3.2f, 0.0f, hd - 0.2f + 0.45f, trim, Shade(trim, 6));
        lbox(1.6f, 0.05f, 3.2f, 4.1f, 0.0f, hd - 0.2f + 0.9f, Col(70, 72, 80, MatMetal), Col(70, 72, 80, MatMetal));
        lbox(0.3f, 0.3f, eave, eave + 2.2f, -1.8f, -1.2f, Col(110, 88, 80, MatBrick), Col(80, 66, 60));
        lbox(3.3f, 0.25f, 0.0f, 0.7f, 0.0f, 3.3f, Col(58, 112, 52, MatFoliage), Col(74, 132, 60, MatFoliage));
      }
      else
      {
        // Apartment block: bevelled shell, floor ledges, cornice, rooftop plant.
        const float hw = 3.2f, hd = 3.0f, h = 15.5f;
        const uint32_t block = (v & 1) ? Col(202, 178, 150, MatBrick) : Col(214, 210, 200, MatPlaster);
        band(hw, hd, 0.0f, 0.5f, 0, 0, 0.12f, Col(120, 118, 114, MatConcrete));
        lshell(hw, hd, 0.45f, h, 0, 0, 0.28f, block, WithMat(Col(96, 98, 104), MatRoof));
        for (int fl = 0; fl < 5; ++fl)
        {
          const float yc = 1.8f + fl * 3.0f;
          windowRow(hw, hd, yc, 4, 0.95f, 1.5f, glass, true, fl == 0 ? 0.9f : 0.0f);
          windowRow(hw, hd, yc, 4, 0.95f, 1.5f, glass, false);
          sideWindows(hw, hd, yc, 3, 0.95f, 1.5f, glass);
          if (fl > 0)
            lbox(hw + 0.15f, 0.35f, yc - 1.0f, yc - 0.85f, 0.0f, hd + 0.35f, trim, Shade(trim, 6));
        }
        door(0.0f, hd, 1.6f, 2.4f, glassDark);
        band(hw, hd, h - 0.4f, h + 0.1f, 0, 0, 0.25f, trim);                                                // cornice
        lbox(hw + 0.1f, hd + 0.1f, h + 0.1f, h + 0.7f, 0, 0, Shade(block, -20), Shade(block, -30));            // parapet
        lshell(1.3f, 1.0f, h, h + 1.8f, -1.2f, 0.8f, 0.1f, Col(150, 150, 156, MatConcrete), Col(170, 170, 176, MatConcrete));
        AddCylinder(l, local(1.8f, h, -1.4f), 0.5f, 0.5f, 1.2f, 10, Col(190, 190, 196, MatMetal), true);
      }
      break;
    }
    case City::Com:
    {
      if (t.level == 1)
      {
        // Corner shop: bevelled plaster box, glass storefront, striped awning, lit sign.
        static const uint32_t walls[3] = { Col(214, 206, 192, MatPlaster), Col(186, 150, 120, MatBrick), Col(200, 208, 210, MatPlaster) };
        static const uint32_t awn[3] = { Col(196, 62, 54), Col(52, 130, 96), Col(220, 150, 40) };
        const uint32_t wall = walls[v % 3];
        const float hw = 3.1f, hd = 2.6f, h = 4.4f;
        lshell(hw, hd, 0.0f, h, 0, -0.3f, 0.16f, wall, WithMat(Col(140, 140, 136), MatRoof));
        band(hw, hd, h - 0.35f, h + 0.1f, 0, -0.3f, 0.22f, trim);
        lbox(hw + 0.12f, hd + 0.12f, h + 0.1f, h + 0.55f, 0, -0.3f, Shade(wall, -24), Shade(wall, -30));
        windowRow(hw, hd - 0.3f, 1.5f, 3, 1.7f, 2.3f, glass, true);
        door(2.05f, hd - 0.3f, 0.9f, 2.4f, glassDark);
        sideWindows(hw, hd, 2.6f, 2, 1.0f, 1.0f, glass);
        for (int i = 0; i < 6; ++i)
        {
          const float x0 = -hw + i * (hw * 2.0f / 6.0f), x1 = x0 + hw * 2.0f / 6.0f;
          const uint32_t c = (i & 1) ? awn[v % 3] : Col(236, 232, 220);
          const Vec3 a = local(x0, 3.2f, hd - 0.3f), b = local(x1, 3.2f, hd - 0.3f);
          const Vec3 c2 = local(x1, 2.6f, hd + 0.9f), d = local(x0, 2.6f, hd + 0.9f);
          Vec3 n = (b - a).Cross(d - a).Normalized();
          if (n.y < 0.0f) n = n * -1.0f;
          AddFace(l, a, b, c2, d, n, c);
          AddFace(l, d, c2, b, a, n * -1.0f, Shade(c, -20));
        }
        const Vec3 signTint = ColVec(awn[v % 3]) * 0.5f + Vec3 { 0.5f, 0.45f, 0.35f };
        lbox(2.2f, 0.12f, 3.4f, 4.1f, 0.0f, hd - 0.3f + 0.15f, Col(255, 236, 190, MatEmissive), Col(255, 236, 190, MatEmissive));
        signLight(local(0.0f, 3.6f, hd), signTint, 14.0f);
        lshell(1.0f, 0.8f, h + 0.1f, h + 0.8f, 1.6f, -1.6f, 0.1f, Col(170, 172, 176, MatMetal), Col(190, 192, 196, MatMetal));
      }
      else if (t.level == 2)
      {
        // Mid-rise office: glass bands between bevelled concrete spandrels.
        const float hw = 3.1f, hd = 3.0f, h = 11.5f;
        const uint32_t spandrel = (v & 1) ? Col(178, 176, 170, MatConcrete) : Col(120, 128, 140, MatConcrete);
        lshell(hw, hd, 0.0f, h, 0, 0, 0.3f, spandrel, WithMat(Col(90, 96, 108), MatRoof));
        for (int fl = 0; fl < 3; ++fl)
        {
          const float y0 = 1.1f + fl * 3.4f;
          lbox(hw + 0.06f, hd + 0.06f, y0, y0 + 2.2f, 0, 0, glass, glass);
        }
        door(0.0f, hd, 2.4f, 2.6f, glassDark);
        lbox(2.0f, 0.9f, 2.9f, 3.1f, 0.0f, hd + 0.8f, Col(70, 74, 84, MatMetal), Col(90, 94, 104, MatMetal));
        band(hw, hd, h - 0.3f, h + 0.1f, 0, 0, 0.25f, Shade(spandrel, 15));
        lbox(hw + 0.12f, hd + 0.12f, h + 0.1f, h + 0.55f, 0, 0, Shade(spandrel, -20), Shade(spandrel, -28));
        lshell(1.4f, 1.0f, h, h + 1.3f, 1.2f, -1.2f, 0.1f, Col(160, 162, 168, MatConcrete), Col(180, 182, 188, MatConcrete));
        AddCylinder(l, local(-1.6f, h, 1.2f), 0.45f, 0.45f, 1.5f, 10, Col(175, 178, 184, MatMetal), true);
        signLight(local(0.0f, 3.0f, hd + 0.8f), { 0.9f, 0.95f, 1.0f }, 8.0f);
      }
      else
      {
        // Tower: curtain wall with strong bevels, setback crown, spire.
        const float hw = 3.1f, hd = 3.1f, h = 24.0f + (v % 5) * 3.0f;
        const uint32_t curtain = WithMat((v & 1) ? Col(108, 148, 192) : Col(128, 164, 198), MatFacade);
        band(hw, hd, 0.0f, 0.6f, 0, 0, 0.15f, Col(70, 74, 84, MatConcrete));
        lshell(hw, hd, 0.5f, h, 0, 0, 0.55f, curtain, WithMat(Col(80, 88, 104), MatRoof));
        for (int i = -1; i <= 1; ++i)
        {
          lbox(0.08f, 0.1f, 0.6f, h, i * 1.55f, hd + 0.05f, Col(60, 66, 78, MatMetal), Col(60, 66, 78, MatMetal));
          lbox(0.08f, 0.1f, 0.6f, h, i * 1.55f, -hd - 0.05f, Col(60, 66, 78, MatMetal), Col(60, 66, 78, MatMetal));
        }
        door(0.0f, hd, 2.6f, 3.2f, glassDark);
        lbox(2.6f, 1.0f, 3.5f, 3.7f, 0.0f, hd + 0.9f, Col(70, 74, 84, MatMetal), Col(90, 94, 104, MatMetal));
        lshell(2.2f, 2.2f, h, h + 4.0f, 0, 0, 0.5f, WithMat(Shade(curtain, -20), MatFacade), WithMat(Col(80, 88, 104), MatRoof));
        AddCylinder(l, local(0, h + 4.0f, 0), 0.25f, 0.06f, 6.0f, 6, Col(60, 60, 64, MatMetal), false);
        AddSphere(l, local(0, h + 10.2f, 0), 0.3f, 6, 3, Col(255, 60, 60, MatEmissive), 1, 0.0f);
        signLight(local(0.0f, 3.6f, hd + 0.9f), { 0.85f, 0.9f, 1.0f }, 10.0f);
      }
      break;
    }
    case City::Ind:
    {
      const uint32_t wall = (v & 1) ? Col(164, 160, 150, MatMetal) : Col(184, 172, 156, MatPlaster);
      const uint32_t roofMetal = Col(150, 150, 156, MatMetal);
      if (t.level == 1)
      {
        // Warehouse: sawtooth roof, roller door, pallets in the yard.
        const float hw = 3.2f, hd = 2.7f, h = 3.6f;
        lshell(hw, hd, 0.0f, h, 0, -0.5f, 0.1f, wall, roofMetal);
        for (int i = 0; i < 2; ++i)
        {
          Vec3 tmn, tmx;
          lrect(hw, hd * 0.5f, 0, -0.5f + (i == 0 ? -hd * 0.5f : hd * 0.5f), tmn, tmx);
          AddGable(l, { tmn.x, base, tmn.z }, { tmx.x, base, tmx.z }, base + h, 1.1f, !alongX, wall, roofMetal, 0.1f);
        }
        door(0.0f, hd - 0.5f, 2.6f, 2.8f, Col(96, 100, 106, MatMetal));
        sideWindows(hw, hd, 2.6f, 2, 1.2f, 0.7f, glassDark);
        lbox(0.6f, 0.6f, 0.0f, 0.9f, 2.4f, 2.5f, Col(170, 130, 80), Col(190, 150, 100));
        lbox(0.6f, 0.6f, 0.0f, 0.5f, -2.4f, 2.6f, Col(170, 130, 80), Col(190, 150, 100));
        lbox(3.4f, 0.05f, 0.0f, 1.2f, 0.0f, 3.4f, Col(120, 122, 126, MatMetal), Col(120, 122, 126, MatMetal));
      }
      else if (t.level == 2)
      {
        // Factory hall with a smokestack, side pipe run and a roof vent.
        const float hw = 3.3f, hd = 3.0f, h = 5.6f;
        Vec3 mn, mx;
        lrect(hw, hd, 0, -0.2f, mn, mx);
        lshell(hw, hd, 0.0f, h, 0, -0.2f, 0.12f, wall, roofMetal);
        AddGable(l, { mn.x, base, mn.z }, { mx.x, base, mx.z }, base + h, 1.4f, alongX, wall, roofMetal, 0.2f);
        door(-1.2f, hd - 0.2f, 2.4f, 3.0f, Col(96, 100, 106, MatMetal));
        windowRow(hw, hd - 0.2f, 4.2f, 4, 0.9f, 0.8f, glassDark, true);
        windowRow(hw, hd - 0.2f, 4.2f, 4, 0.9f, 0.8f, glassDark, false);
        AddCylinder(l, local(2.4f, 0.0f, -2.2f), 0.55f, 0.42f, 10.5f, 10, Col(150, 92, 76, MatBrick), true);
        AddCylinder(l, local(2.4f, 10.5f, -2.2f), 0.5f, 0.5f, 0.5f, 10, Col(60, 56, 54, MatMetal), true);
        AddCylinderAxis(l, local(-hw - 0.3f, 2.2f, 0.0f), f, 0.18f, hd * 1.8f, 8, Col(120, 124, 130, MatMetal));
        lbox(0.8f, 0.8f, h + 1.4f, h + 2.2f, -1.4f, 0.6f, Col(180, 182, 186, MatMetal), Col(196, 198, 202, MatMetal));
      }
      else
      {
        // Heavy plant: main hall, two stacks, storage tanks, conveyor gantry.
        const float hw = 3.4f, hd = 3.2f, h = 8.0f;
        Vec3 mn, mx;
        lrect(hw, hd, 0, 0, mn, mx);
        lshell(hw, hd, 0.0f, h, 0, 0, 0.15f, wall, roofMetal);
        AddGable(l, { mn.x, base, mn.z }, { mx.x, base, mx.z }, base + h, 1.2f, alongX, wall, roofMetal, 0.2f);
        AddCylinder(l, local(2.3f, 0.0f, -2.3f), 0.6f, 0.45f, 14.0f, 10, Col(150, 92, 76, MatBrick), true);
        AddCylinder(l, local(-2.3f, 0.0f, -2.3f), 0.5f, 0.4f, 12.0f, 10, Col(150, 92, 76, MatBrick), true);
        AddCylinder(l, local(-2.4f, 0.0f, 2.4f), 0.9f, 0.9f, 3.2f, 12, Col(196, 198, 204, MatMetal), true);
        AddCylinder(l, local(-0.4f, 0.0f, 2.6f), 0.7f, 0.7f, 2.6f, 12, Col(190, 170, 130, MatMetal), true);
        AddCylinderAxis(l, local(hw + 0.4f, 3.2f, 0.0f), f, 0.22f, 7.2f, 8, Col(120, 124, 130, MatMetal));
        lbox(0.12f, 0.12f, 0.0f, 3.0f, hw + 0.4f, -2.8f, Col(100, 104, 110, MatMetal), Col(100, 104, 110, MatMetal));
        lbox(0.12f, 0.12f, 0.0f, 3.0f, hw + 0.4f, 2.8f, Col(100, 104, 110, MatMetal), Col(100, 104, 110, MatMetal));
        windowRow(hw, hd, 6.4f, 5, 0.9f, 0.8f, glassDark, true);
        door(1.0f, hd, 2.6f, 3.4f, Col(96, 100, 106, MatMetal));
        AddSphere(l, local(2.3f, 14.3f, -2.3f), 0.3f, 6, 3, Col(255, 80, 60, MatEmissive), 1, 0.0f);
        signLight(local(0.0f, 5.0f, hd + 0.2f), { 1.0f, 0.9f, 0.7f }, 10.0f);
      }
      break;
    }
    default:
      break;
  }

  // Spawn animation: everything above the lot scales up from the ground.
  if (rise < 0.999f)
  {
    for (size_t i = firstVertex; i < l.vertices.size(); ++i)
      l.vertices[i].py = base + (l.vertices[i].py - base) * rise;
    if (m_spotSink)
      m_spotSink->resize(firstLight); // no lights until it stands
  }
}

// ============================================================================
// Service buildings
// ============================================================================

void CityRenderer::AddServiceBuilding(Layer& l, const City& city, int x, int y, float rise)
{
  const City::Tile& t = city.At(x, y);
  const int kind = t.type == City::Service ? t.service : -1;
  AddServiceModel(l, city, x, y, kind, t.variant, rise);
}

void CityRenderer::AddServiceModel(Layer& l, const City& city, int x, int y, int kind, int v, float rise)
{
  if (kind < 0 || kind >= City::ServiceCount)
    return;
  const float base = city.TileTop(x, y);
  const float cx = (x + 0.5f) * kCell, cz = (y + 0.5f) * kCell;
  const size_t firstVertex = l.vertices.size();
  const size_t firstLight = m_spotSink ? m_spotSink->size() : 0;
  Vec3 f { 0, 0, 1 };
  if (city.IsRoad(x, y + 1)) f = { 0, 0, 1 };
  else if (city.IsRoad(x, y - 1)) f = { 0, 0, -1 };
  else if (city.IsRoad(x + 1, y)) f = { 1, 0, 0 };
  else if (city.IsRoad(x - 1, y)) f = { -1, 0, 0 };
  // Authored model when the library has one for this service.
  static const char* kServiceAssets[City::ServiceCount] = { "svc_power", "svc_water",
    "svc_clinic", "svc_school", "svc_police", "svc_park", "svc_transit" };
  if (m_assets.IsLoaded())
  {
    if (const CityAssets::Model* model = m_assets.Find(kServiceAssets[kind]))
    {
      AddModel(l, *model, { cx, base, cz }, f, 1.0f, rise);
      if (m_spotSink && rise > 0.999f)
      {
        // Every service lights its own forecourt after dark.
        const Vec3 pos = Vec3 { cx, base + model->height * 0.75f, cz } + f * 2.2f;
        AddSpot(pos, Vec3 { 0, -1, 0 } + f * 0.25f, { 1.0f, 0.95f, 0.85f },
          kind == City::Park ? 18.0f : 22.0f, kind == City::Park ? 12.0f : 14.0f, 35.0f, 60.0f);
      }
      return;
    }
  }

  const Vec3 r = Vec3 { 0, 1, 0 }.Cross(f);
  const Vec3 up { 0, 1, 0 };
  auto local = [&](float lx, float ly, float lz) { return Vec3 { cx, base, cz } + r * lx + up * ly + f * lz; };
  auto lbox = [&](float hw, float hd, float y0, float y1, float ox, float oz, uint32_t side, uint32_t top) {
    const Vec3 a = local(ox - hw, y0, oz - hd), b = local(ox + hw, y1, oz + hd);
    AddBox(l, { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) },
      { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }, side, top);
  };
  auto lshell = [&](float hw, float hd, float y0, float y1, float ox, float oz, float bevel, uint32_t side, uint32_t top) {
    const Vec3 a = local(ox - hw, y0, oz - hd), b = local(ox + hw, y1, oz + hd);
    AddChamferBox(l, { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) },
      { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }, bevel, side, top);
  };
  auto windowRow = [&](float hw, float hd, float yc, int count, float w, float h, uint32_t glass, bool front) {
    const Vec3 n = front ? f : f * -1.0f;
    const Vec3 right = front ? r : r * -1.0f;
    for (int i = 0; i < count; ++i)
    {
      const float lx = -hw + (hw * 2.0f) * (i + 0.5f) / count;
      AddWindow(l, local(lx, yc, front ? hd : -hd), n, right, w, h, glass, Col(40, 42, 48));
    }
  };
  auto door = [&](float lx, float hd, float w, float h, uint32_t col) {
    AddWindow(l, local(lx, h * 0.5f + 0.05f, hd), f, r, w, h, col, Col(60, 50, 44));
  };
  auto floodlight = [&](Vec3 pos, Vec3 dir, Vec3 tint, float intensity, float range) {
    AddSpot(pos, dir, tint, intensity, range, 35.0f, 60.0f);
  };
  const uint32_t glass = Col(150, 188, 220, MatWindow);
  const uint32_t trim = Col(226, 222, 212, MatConcrete);
  const uint32_t white = Col(236, 236, 232, MatPlaster);
  const uint32_t concrete = Col(178, 176, 170, MatConcrete);

  switch (kind)
  {
    case City::PowerPlant:
    {
      // Turbine hall, two cooling towers, a tall stack, transformer yard.
      lshell(2.6f, 1.6f, 0.0f, 5.5f, -0.9f, 1.6f, 0.15f, Col(158, 152, 142, MatMetal), Col(140, 140, 146, MatMetal));
      windowRow(2.6f, 1.6f, 3.8f, 4, 0.9f, 0.9f, Col(96, 130, 160, MatWindow), true);
      door(-0.5f, 3.2f, 2.2f, 3.0f, Col(96, 100, 106, MatMetal));
      for (int i = 0; i < 2; ++i)
      {
        const Vec3 c = local(1.6f + i * 0.0f, 0.0f, -1.7f + i * 0.0f);
        (void)c;
      }
      AddCylinder(l, local(1.9f, 0.0f, -1.6f), 1.75f, 1.15f, 7.0f, 14, Col(196, 192, 184, MatConcrete), false);
      AddCylinder(l, local(1.9f, 7.0f, -1.6f), 1.15f, 1.3f, 2.2f, 14, Col(196, 192, 184, MatConcrete), false);
      AddCylinder(l, local(-2.2f, 0.0f, -1.8f), 1.35f, 0.9f, 5.5f, 14, Col(196, 192, 184, MatConcrete), false);
      AddCylinder(l, local(-2.2f, 5.5f, -1.8f), 0.9f, 1.0f, 1.8f, 14, Col(196, 192, 184, MatConcrete), false);
      AddCylinder(l, local(-2.9f, 0.0f, 2.6f), 0.45f, 0.35f, 15.0f, 10, Col(150, 92, 76, MatBrick), true);
      for (int i = 0; i < 3; ++i)
        lbox(0.3f, 0.3f, 0.0f, 1.4f, 2.2f + i * 0.0f - i * 0.9f, 2.9f, Col(120, 124, 130, MatMetal), Col(140, 144, 150, MatMetal));
      lbox(3.6f, 0.04f, 0.0f, 1.1f, 0.0f, 3.5f, Col(120, 122, 126, MatMetal), Col(120, 122, 126, MatMetal));
      AddSphere(l, local(-2.9f, 15.3f, 2.6f), 0.28f, 6, 3, Col(255, 60, 60, MatEmissive), 1, 0.0f);
      floodlight(local(0.0f, 5.4f, 3.2f), { 0, -1, 0.3f }, { 1.0f, 0.95f, 0.85f }, 30.0f, 14.0f);
      break;
    }
    case City::WaterTower:
    {
      // Tank on four legs with a conical cap, pump house beside it.
      const uint32_t steel = Col(196, 200, 206, MatMetal);
      for (int i = 0; i < 4; ++i)
      {
        const float lx = (i & 1) ? 1.5f : -1.5f, lz = (i & 2) ? 1.5f : -1.5f;
        const Vec3 foot = local(lx, 0.0f, lz - 0.6f), head = local(lx * 0.75f, 8.5f, lz * 0.75f - 0.6f);
        AddCylinderAxis(l, (foot + head) * 0.5f, head - foot, 0.11f, (head - foot).Length(), 6, Col(90, 94, 100, MatMetal));
      }
      lbox(1.6f, 0.06f, 4.0f, 4.12f, 0.0f, -0.6f, Col(90, 94, 100, MatMetal), Col(90, 94, 100, MatMetal));
      lbox(0.06f, 1.6f, 4.0f, 4.12f, 0.0f, -0.6f, Col(90, 94, 100, MatMetal), Col(90, 94, 100, MatMetal));
      AddCylinder(l, local(0.0f, 8.3f, -0.6f), 1.9f, 2.2f, 1.2f, 16, steel, false);
      AddCylinder(l, local(0.0f, 9.5f, -0.6f), 2.2f, 2.2f, 2.8f, 16, steel, false);
      AddCylinder(l, local(0.0f, 12.3f, -0.6f), 2.3f, 0.2f, 1.6f, 16, Col(120, 132, 150, MatMetal), true);
      lbox(2.2f, 0.2f, 10.2f, 11.4f, 0.0f, -0.6f + 2.25f, Col(60, 120, 200, MatPaint), Col(60, 120, 200, MatPaint)); // blue band
      lshell(1.8f, 1.2f, 0.0f, 2.8f, 0.0f, 2.4f, 0.12f, Col(212, 204, 190, MatBrick), Col(90, 96, 104, MatRoof));
      door(0.0f, 3.6f, 1.0f, 2.1f, Col(96, 100, 106, MatMetal));
      AddCylinderAxis(l, local(0.0f, 1.4f, 0.9f), f, 0.16f, 3.0f, 8, Col(90, 94, 100, MatMetal));
      floodlight(local(0.0f, 13.6f, -0.6f), { 0, -1, 0 }, { 0.8f, 0.9f, 1.0f }, 14.0f, 16.0f);
      break;
    }
    case City::Clinic:
    {
      // Two-storey white block, glazed entrance, red cross, ambulance bay.
      lshell(3.2f, 2.6f, 0.0f, 7.4f, 0.0f, -0.5f, 0.28f, white, WithMat(Col(150, 156, 164), MatRoof));
      lbox(3.35f, 2.75f, 3.5f, 3.7f, 0.0f, -0.5f, trim, Shade(trim, 6));
      lbox(3.35f, 2.75f, 7.1f, 7.5f, 0.0f, -0.5f, trim, Shade(trim, 6));
      for (int fl = 0; fl < 2; ++fl)
      {
        windowRow(3.2f, 2.6f, 1.9f + fl * 3.6f, 4, 1.1f, 1.4f, glass, true);
        windowRow(3.2f, 2.6f, 1.9f + fl * 3.6f, 4, 1.1f, 1.4f, glass, false);
      }
      door(0.0f, 2.1f, 2.2f, 2.6f, Col(96, 130, 160, MatWindow));
      lbox(2.0f, 1.2f, 2.9f, 3.1f, 0.0f, 3.0f, Col(200, 204, 210, MatMetal), Col(210, 214, 220, MatMetal)); // canopy
      const uint32_t red = Col(230, 50, 50, MatEmissive);
      lbox(0.9f, 0.1f, 5.4f, 5.9f, 0.0f, 2.15f, red, red);
      lbox(0.25f, 0.1f, 4.75f, 6.55f, 0.0f, 2.15f, red, red);
      lbox(1.1f, 0.9f, 0.0f, 1.1f, 2.6f, 2.9f, Col(240, 240, 240, MatPaint), Col(240, 240, 240, MatPaint)); // ambulance
      lbox(0.9f, 0.15f, 0.5f, 0.9f, 2.6f, 3.85f, Col(230, 50, 50, MatPaint), Col(230, 50, 50, MatPaint));
      floodlight(local(0.0f, 3.0f, 3.0f), { 0, -1, 0.2f }, { 1.0f, 1.0f, 1.0f }, 16.0f, 10.0f);
      AddSpot(local(0.0f, 5.8f, 2.6f), f + Vec3 { 0, -0.3f, 0 }, { 1.0f, 0.3f, 0.3f }, 10.0f, 9.0f, 45.0f, 75.0f);
      break;
    }
    case City::School:
    {
      // Brick L-shaped school, gabled roof, flagpole, playground.
      const uint32_t brick = Col(176, 104, 84, MatBrick), roof = Col(72, 74, 82, MatRoof);
      Vec3 mn, mx;
      {
        const Vec3 a = local(-3.4f, 0, -3.2f), b = local(3.4f, 0, -0.8f);
        mn = { std::min(a.x, b.x), base, std::min(a.z, b.z) };
        mx = { std::max(a.x, b.x), base, std::max(a.z, b.z) };
      }
      lshell(3.4f, 1.2f, 0.0f, 4.2f, 0.0f, -2.0f, 0.15f, brick, roof);
      AddGable(l, mn, mx, base + 4.2f, 1.5f, f.z != 0.0f, brick, roof, 0.35f);
      lshell(1.3f, 1.6f, 0.0f, 4.2f, -2.1f, 0.8f, 0.15f, brick, roof);
      lbox(1.4f, 1.7f, 4.2f, 4.5f, -2.1f, 0.8f, trim, Shade(trim, 6));
      windowRow(3.4f, 1.2f, 2.2f, 5, 1.0f, 1.3f, glass, true);
      windowRow(3.4f, 1.2f, 2.2f, 5, 1.0f, 1.3f, glass, false);
      door(-2.1f, 2.4f, 1.6f, 2.4f, Col(96, 130, 160, MatWindow));
      AddCylinder(l, local(1.5f, 0.0f, 2.8f), 0.05f, 0.05f, 6.0f, 6, Col(220, 220, 224, MatMetal), false);
      lbox(0.45f, 0.02f, 5.2f, 5.8f, 1.95f, 2.8f, Col(230, 60, 60, MatPaint), Col(230, 60, 60, MatPaint)); // flag
      for (int i = 0; i < 3; ++i)
        lbox(0.35f, 0.35f, 0.0f, 0.5f + i * 0.3f, 0.6f + i * 0.9f, 1.8f, Col(230, 180, 60, MatPaint), Col(90, 150, 220, MatPaint));
      AddTree(l, local(3.0f, 0.0f, 2.6f).x, base, local(3.0f, 0.0f, 2.6f).z, 0.6f, static_cast<uint32_t>(v) * 3u + 2u);
      floodlight(local(-2.1f, 3.2f, 2.6f), { 0, -1, 0.2f }, { 1.0f, 0.95f, 0.85f }, 12.0f, 9.0f);
      break;
    }
    case City::PoliceStation:
    {
      // Blue-grey block with a glazed lobby, antenna mast and patrol cars.
      const uint32_t wall = Col(150, 160, 176, MatConcrete);
      lshell(3.0f, 2.4f, 0.0f, 6.8f, 0.0f, -0.6f, 0.3f, wall, WithMat(Col(96, 100, 110), MatRoof));
      lbox(3.15f, 2.55f, 6.5f, 6.9f, 0.0f, -0.6f, trim, Shade(trim, 6));
      for (int fl = 0; fl < 2; ++fl)
      {
        windowRow(3.0f, 2.4f, 1.8f + fl * 3.2f, 4, 1.0f, 1.2f, glass, true);
        windowRow(3.0f, 2.4f, 1.8f + fl * 3.2f, 4, 1.0f, 1.2f, glass, false);
      }
      door(0.0f, 1.8f, 2.4f, 2.6f, Col(96, 130, 160, MatWindow));
      const uint32_t blue = Col(70, 130, 255, MatEmissive);
      lbox(1.6f, 0.08f, 3.1f, 3.6f, 0.0f, 1.85f, blue, blue); // POLICE sign
      AddCylinder(l, local(-2.2f, 6.8f, -1.8f), 0.08f, 0.04f, 5.0f, 6, Col(120, 124, 130, MatMetal), false);
      for (int i = 0; i < 2; ++i)
      {
        const float ox = 1.8f - i * 1.6f;
        lbox(0.6f, 1.0f, 0.0f, 0.75f, ox, 2.9f, Col(240, 240, 244, MatPaint), Col(40, 60, 110, MatPaint));
        lbox(0.5f, 0.55f, 0.75f, 1.25f, ox, 2.8f, Col(60, 70, 90, MatWindow), Col(240, 240, 244, MatPaint));
        lbox(0.35f, 0.08f, 1.25f, 1.4f, ox, 2.8f, Col(255, 60, 60, MatEmissive), Col(70, 130, 255, MatEmissive));
      }
      AddSpot(local(0.0f, 3.4f, 1.9f), f + Vec3 { 0, -0.4f, 0 }, { 0.4f, 0.6f, 1.0f }, 14.0f, 10.0f, 45.0f, 75.0f);
      floodlight(local(0.0f, 6.9f, -0.6f), { 0, -1, 0 }, { 1.0f, 1.0f, 1.0f }, 10.0f, 12.0f);
      break;
    }
    case City::Park:
    {
      // Lawn, crossing paths, a pond, a fountain, benches and trees.
      const float x0 = x * kCell + 0.4f, x1 = x0 + kCell - 0.8f, z0 = y * kCell + 0.4f, z1 = z0 + kCell - 0.8f;
      AddFace(l, { x0, base + 0.06f, z0 }, { x1, base + 0.06f, z0 }, { x1, base + 0.06f, z1 }, { x0, base + 0.06f, z1 }, up, Col(88, 150, 66, MatGrass));
      lbox(3.6f, 0.4f, 0.06f, 0.1f, 0.0f, 0.0f, Col(196, 186, 168, MatConcrete), Col(196, 186, 168, MatConcrete));
      lbox(0.4f, 3.6f, 0.06f, 0.1f, 0.0f, 0.0f, Col(196, 186, 168, MatConcrete), Col(196, 186, 168, MatConcrete));
      AddCylinder(l, local(0.0f, 0.1f, 0.0f), 1.3f, 1.3f, 0.35f, 14, Col(200, 196, 188, MatConcrete), false);
      AddCylinder(l, local(0.0f, 0.1f, 0.0f), 1.1f, 1.1f, 0.3f, 14, Col(60, 120, 170, MatWater), true);
      AddCylinder(l, local(0.0f, 0.4f, 0.0f), 0.3f, 0.25f, 1.1f, 10, Col(200, 196, 188, MatConcrete), true);
      AddSphere(l, local(0.0f, 1.9f, 0.0f), 0.45f, 8, 4, Col(190, 214, 236, MatWindow), 1, 0.15f);
      const float tx[4] = { -2.6f, 2.6f, -2.6f, 2.6f }, tz[4] = { -2.6f, -2.6f, 2.6f, 2.6f };
      for (int i = 0; i < 4; ++i)
      {
        const Vec3 p = local(tx[i], 0.0f, tz[i]);
        AddTree(l, p.x, base, p.z, 0.7f + (Hash8(i, v, 1) % 20) * 0.01f, static_cast<uint32_t>(v) + i * 13u);
      }
      for (int i = 0; i < 2; ++i)
      {
        const float ox = i ? 1.9f : -1.9f;
        lbox(0.6f, 0.2f, 0.4f, 0.5f, ox, 1.1f, Col(120, 88, 60), Col(140, 104, 72));
        lbox(0.6f, 0.05f, 0.5f, 0.95f, ox, 1.3f, Col(120, 88, 60), Col(140, 104, 72));
      }
      AddCylinder(l, local(1.8f, 0.1f, -1.4f), 0.06f, 0.05f, 3.2f, 6, Col(60, 62, 68, MatMetal), false);
      AddSphere(l, local(1.8f, 3.4f, -1.4f), 0.22f, 6, 3, Col(255, 236, 190, MatEmissive), 1, 0.0f);
      floodlight(local(1.8f, 3.2f, -1.4f), { 0, -1, 0 }, { 1.0f, 0.9f, 0.7f }, 18.0f, 12.0f);
      break;
    }
    case City::BusDepot:
    {
      // Long garage hall, glazed office corner, buses lined up in the yard.
      const uint32_t wall = Col(170, 174, 180, MatMetal);
      lshell(3.4f, 1.8f, 0.0f, 4.6f, 0.0f, -1.8f, 0.12f, wall, Col(150, 150, 156, MatMetal));
      lshell(1.5f, 1.3f, 0.0f, 3.2f, 2.0f, 1.5f, 0.15f, Col(214, 206, 192, MatPlaster), WithMat(Col(96, 98, 104), MatRoof));
      windowRow(1.5f, 1.3f, 1.8f, 2, 1.0f, 1.2f, glass, true);
      door(2.0f, 2.8f, 1.0f, 2.2f, Col(96, 130, 160, MatWindow));
      for (int i = 0; i < 3; ++i)
        door(-2.4f + i * 1.9f, 0.0f, 1.6f, 3.6f, Col(96, 100, 106, MatMetal));
      for (int i = 0; i < 2; ++i)
      {
        const float ox = -2.4f + i * 1.6f;
        lbox(0.55f, 2.0f, 0.0f, 1.9f, ox, 1.6f, Col(240, 200, 60, MatPaint), Col(250, 220, 90, MatPaint));
        lbox(0.57f, 1.8f, 0.9f, 1.6f, ox, 1.6f, Col(60, 70, 90, MatWindow), Col(250, 220, 90, MatPaint));
      }
      lbox(1.4f, 0.1f, 3.2f, 3.7f, 2.0f, 2.85f, Col(90, 220, 200, MatEmissive), Col(90, 220, 200, MatEmissive));
      AddSpot(local(2.0f, 3.5f, 2.9f), f + Vec3 { 0, -0.5f, 0 }, { 0.5f, 1.0f, 0.9f }, 10.0f, 9.0f, 45.0f, 75.0f);
      floodlight(local(0.0f, 4.6f, -1.0f), { 0, -1, 0.5f }, { 1.0f, 0.95f, 0.85f }, 24.0f, 14.0f);
      break;
    }
    default:
      break;
  }

  if (rise < 0.999f)
  {
    for (size_t i = firstVertex; i < l.vertices.size(); ++i)
      l.vertices[i].py = base + (l.vertices[i].py - base) * rise;
    if (m_spotSink)
      m_spotSink->resize(firstLight);
  }
}

// ============================================================================
// Vehicles and boats
// ============================================================================

void CityRenderer::AddCar(Layer& l, Vec3 c, Vec3 dir, uint32_t color, bool night)
{
  const Vec3 d = dir.Normalized();
  if (m_assets.IsLoaded())
  {
    // The van shape stands in for a fraction of the traffic.
    const bool van = ((color >> 8) & 1) != 0;
    if (const CityAssets::Model* model = m_assets.Find(van ? "car_van" : "car_sedan"))
    {
      const size_t first = l.vertices.size();
      AddModel(l, *model, c, d);
      // Recolour the painted body to this vehicle's colour; the model's other
      // materials (glass, tyres, lights) keep what Blender authored.
      for (size_t i = first; i < l.vertices.size(); ++i)
      {
        Vertex& v = l.vertices[i];
        if ((v.rgba >> 24) == MatPaint)
          v.rgba = WithMat(color, MatPaint);
      }
      if (night && m_spotSink)
      {
        const Vec3 right = d.Cross(Vec3 { 0, 1, 0 });
        for (int side = -1; side <= 1; side += 2)
          AddSpot(c + d * 2.1f + right * (0.6f * side) + Vec3 { 0, 0.7f, 0 },
            d + Vec3 { 0, -0.22f, 0 }, { 1.0f, 0.95f, 0.8f }, 28.0f, 16.0f, 14.0f, 30.0f);
      }
      return;
    }
  }
  const Vec3 right = d.Cross(Vec3 { 0, 1, 0 });
  const Vec3 up { 0, 1, 0 };
  auto P = [&](float along, float y, float side) { return c + d * along + up * y + right * side; };
  // Lower body: bevelled octagonal prism in the car's frame.
  auto prism = [&](float a0, float a1, float s, float y0, float y1, float bev, uint32_t side, uint32_t top) {
    Vec3 ring[8] = { P(a0 + bev, 0, -s), P(a1 - bev, 0, -s), P(a1, 0, -s + bev), P(a1, 0, s - bev),
      P(a1 - bev, 0, s), P(a0 + bev, 0, s), P(a0, 0, s - bev), P(a0, 0, -s + bev) };
    Vec3 inner[8];
    for (int i = 0; i < 8; ++i)
      inner[i] = ring[i] + (c - ring[i]).Normalized() * bev * 0.8f;
    for (int i = 0; i < 8; ++i)
    {
      const Vec3 a = ring[i], b = ring[(i + 1) % 8];
      Vec3 n = Vec3 { b.z - a.z, 0.0f, -(b.x - a.x) }.Normalized();
      if (n.Dot((a + b) * 0.5f - c) < 0.0f) n = n * -1.0f;
      AddFace(l, a + up * y0, b + up * y0, b + up * (y1 - bev), a + up * (y1 - bev), n, Shade(side, static_cast<int>(-6 + 6 * n.Dot(d))));
      const Vec3 ia = inner[i], ib = inner[(i + 1) % 8];
      AddFace(l, a + up * (y1 - bev), b + up * (y1 - bev), ib + up * y1, ia + up * y1, (n + up).Normalized(), Shade(side, 8));
    }
    const Vec3 ctr = c + up * y1;
    for (int i = 0; i < 8; ++i)
      AddTri(l, ctr, inner[i] + up * y1, inner[(i + 1) % 8] + up * y1, top);
  };
  const uint32_t paint = WithMat(color, MatPaint);
  prism(-2.0f, 2.0f, 0.9f, 0.32f, 1.0f, 0.18f, paint, Shade(paint, 6));
  prism(-1.3f, 0.9f, 0.78f, 1.0f, 1.55f, 0.16f, Col(60, 74, 96, MatWindow), Shade(paint, -6));
  // Wheels.
  for (int i = 0; i < 4; ++i)
  {
    const float along = (i & 1) ? 1.3f : -1.3f, side = (i & 2) ? 0.82f : -0.82f;
    AddCylinderAxis(l, P(along, 0.34f, side), right, 0.34f, 0.28f, 10, Col(28, 28, 30));
    AddCylinderAxis(l, P(along, 0.34f, side * 1.02f), right, 0.16f, 0.3f, 8, Col(190, 192, 198, MatMetal));
  }
  // Lights.
  const uint32_t head = night ? Col(255, 245, 200, MatEmissive) : Col(230, 230, 220, MatWindow);
  const uint32_t tail = night ? Col(255, 40, 30, MatEmissive) : Col(170, 40, 40, MatPaint);
  for (int s = -1; s <= 1; s += 2)
  {
    const Vec3 hp = P(2.0f, 0.7f, s * 0.6f), tp = P(-2.0f, 0.7f, s * 0.6f);
    AddBox(l, { hp.x - 0.12f, hp.y - 0.1f, hp.z - 0.12f }, { hp.x + 0.12f, hp.y + 0.1f, hp.z + 0.12f }, head, head);
    AddBox(l, { tp.x - 0.12f, tp.y - 0.08f, tp.z - 0.12f }, { tp.x + 0.12f, tp.y + 0.08f, tp.z + 0.12f }, tail, tail);
    if (night)
      AddSpot(hp + d * 0.2f, d + Vec3 { 0, -0.22f, 0 }, { 1.0f, 0.95f, 0.8f }, 28.0f, 16.0f, 14.0f, 30.0f);
  }
}

void CityRenderer::AddBoat(Layer& l, Vec3 c, float heading, uint32_t color, float bob)
{
  const Vec3 d { std::cos(heading), 0.0f, std::sin(heading) };
  if (m_assets.IsLoaded())
  {
    if (const CityAssets::Model* model = m_assets.Find("boat"))
    {
      const size_t first = l.vertices.size();
      AddModel(l, *model, c, d);
      for (size_t i = first; i < l.vertices.size(); ++i)
      {
        Vertex& v = l.vertices[i];
        if ((v.rgba >> 24) == MatPaint)
          v.rgba = WithMat(color, MatPaint);
      }
      if (m_spotSink)
        AddSpot(c + Vec3 { 0, 3.7f, 0 }, { 0, -1, 0 }, { 1.0f, 0.95f, 0.8f }, 7.0f, 9.0f, 50.0f, 80.0f);
      return;
    }
  }
  const Vec3 right = d.Cross(Vec3 { 0, 1, 0 });
  const Vec3 up { 0, 1, 0 };
  const float roll = std::sin(bob * 1.7f) * 0.03f;
  auto P = [&](float along, float y, float side) { return c + d * along + up * (y + side * roll) + right * side; };
  // Hull: pointed pentagonal prism, deck on top.
  const Vec3 ring[5] = { P(-3.0f, 0, -1.1f), P(1.2f, 0, -1.1f), P(3.6f, 0, 0.0f), P(1.2f, 0, 1.1f), P(-3.0f, 0, 1.1f) };
  const uint32_t hull = WithMat(color, MatPaint);
  for (int i = 0; i < 5; ++i)
  {
    const Vec3 a = ring[i], b = ring[(i + 1) % 5];
    Vec3 n = Vec3 { b.z - a.z, 0.0f, -(b.x - a.x) }.Normalized();
    if (n.Dot((a + b) * 0.5f - c) < 0.0f) n = n * -1.0f;
    // flare: the hull leans outward towards the deck
    AddFace(l, a - up * 0.6f + n * -0.25f, b - up * 0.6f + n * -0.25f, b + up * 0.55f, a + up * 0.55f, (n + up * 0.2f).Normalized(), hull);
  }
  const Vec3 dc = c + up * 0.55f;
  for (int i = 0; i < 5; ++i)
    AddTri(l, dc, ring[i] + up * 0.55f, ring[(i + 1) % 5] + up * 0.55f, Col(210, 196, 160));
  // Gunwale stripe.
  for (int i = 0; i < 5; ++i)
  {
    const Vec3 a = ring[i], b = ring[(i + 1) % 5];
    Vec3 n = Vec3 { b.z - a.z, 0.0f, -(b.x - a.x) }.Normalized();
    if (n.Dot((a + b) * 0.5f - c) < 0.0f) n = n * -1.0f;
    AddFace(l, a + up * 0.55f + n * 0.03f, b + up * 0.55f + n * 0.03f, b + up * 0.7f + n * 0.03f, a + up * 0.7f + n * 0.03f, n, Col(50, 50, 56, MatPaint));
  }
  // Cabin with windows, mast, stern light.
  const Vec3 cab0 = P(-1.6f, 0.55f, -0.8f), cab1 = P(0.4f, 1.75f, 0.8f);
  AddChamferBox(l, { std::min(cab0.x, cab1.x), cab0.y, std::min(cab0.z, cab1.z) }, { std::max(cab0.x, cab1.x), cab1.y, std::max(cab0.z, cab1.z) },
    0.12f, Col(236, 236, 232, MatPlaster), Col(200, 200, 204, MatPaint));
  const Vec3 w0 = P(-1.4f, 1.0f, -0.83f), w1 = P(0.2f, 1.5f, 0.83f);
  AddBox(l, { std::min(w0.x, w1.x), w0.y, std::min(w0.z, w1.z) }, { std::max(w0.x, w1.x), w1.y, std::max(w0.z, w1.z) },
    Col(60, 74, 96, MatWindow), Col(60, 74, 96, MatWindow));
  AddCylinder(l, P(1.0f, 0.55f, 0.0f), 0.06f, 0.04f, 3.2f, 6, Col(120, 124, 130, MatMetal), false);
  AddSphere(l, P(1.0f, 3.8f, 0.0f), 0.12f, 5, 2, Col(255, 240, 200, MatEmissive), 1, 0.0f);
  AddSpot(P(1.0f, 3.7f, 0.0f), { 0, -1, 0 }, { 1.0f, 0.95f, 0.8f }, 7.0f, 9.0f, 50.0f, 80.0f);
}

// ============================================================================
// Overlay, sky, vehicles, tool preview, selection
// ============================================================================

void CityRenderer::BuildOverlay(const City& city, const ToolPreview& tool)
{
  Layer& l = m_overlay;
  l.vertices.clear();
  l.indices.clear();
  if (tool.overlay >= 0 && tool.overlay < City::ServiceCount)
  {
    static const uint32_t tints[City::ServiceCount] = { Col(255, 214, 80, MatOverlay), Col(90, 170, 255, MatOverlay),
      Col(255, 110, 100, MatOverlay), Col(190, 140, 255, MatOverlay), Col(110, 150, 255, MatOverlay),
      Col(140, 230, 110, MatOverlay), Col(110, 230, 200, MatOverlay) };
    const uint8_t bit = static_cast<uint8_t>(1u << tool.overlay);
    const uint32_t tint = tints[tool.overlay];
    for (int y = 0; y < City::N; ++y)
      for (int x = 0; x < City::N; ++x)
      {
        const City::Tile& t = city.At(x, y);
        if (!(t.coverage & bit))
          continue;
        const float x0 = x * kCell, x1 = x0 + kCell, z0 = y * kCell, z1 = z0 + kCell;
        const float lift = t.type == City::Road ? 0.3f : 0.1f;
        const float h00 = city.RoadCornerHeight(x, y) + lift, h10 = city.RoadCornerHeight(x + 1, y) + lift;
        const float h01 = city.RoadCornerHeight(x, y + 1) + lift, h11 = city.RoadCornerHeight(x + 1, y + 1) + lift;
        const uint32_t c = t.type == City::Service && t.service == tool.overlay ? Shade(tint, 40) : tint;
        AddGroundQuad(l, { x0, h00, z0 }, { x1, h10, z0 }, { x1, h11, z1 }, { x0, h01, z1 }, c);
      }
  }
  l.dirtyMask = kAllSlots;
}

void CityRenderer::BuildDynamic(const City& city, const Camera& camera, const ToolPreview& tool,
  float realTime)
{
  NS_PROFILE_SCOPE("City::BuildDynamic");
  Layer& l = m_dynamic;
  l.vertices.clear();
  l.indices.clear();

  AddSkyDome(l, camera.Eye(), city.day);
  const DayLight L = ComputeDayLight(city.day);
  const bool night = L.night > 0.3f;

  // Buildings still rising (few at a time) regenerate here every frame.
  if (city.gameSeconds < city.animatingUntil)
    for (int y = 0; y < City::N; ++y)
      for (int x = 0; x < City::N; ++x)
      {
        const City::Tile& t = city.At(x, y);
        if (t.type < City::Res || t.level == 0 || city.gameSeconds - t.spawnTime >= City::kSpawnAnimSeconds)
          continue;
        const float k = Clamp((city.gameSeconds - t.spawnTime) / City::kSpawnAnimSeconds, 0.0f, 1.0f);
        const float rise = 1.0f - std::pow(1.0f - k, 3.0f);
        if (t.type == City::Service)
          AddServiceBuilding(l, city, x, y, std::max(rise, 0.02f));
        else
          AddBuilding(l, city, x, y, std::max(rise, 0.02f));
      }

  // Service problems: a blinking marker over buildings without power / water.
  if (std::sin(realTime * 5.0f) > -0.2f)
    for (int y = 0; y < City::N; ++y)
      for (int x = 0; x < City::N; ++x)
      {
        const City::Tile& t = city.At(x, y);
        if (t.type < City::Res || t.type > City::Ind || t.level == 0 || (t.powered && t.watered))
          continue;
        const float top = city.TileTop(x, y) + (t.level == 1 ? 7.0f : t.level == 2 ? 10.0f : 20.0f) + std::sin(realTime * 2.0f) * 0.3f;
        const float cx = (x + 0.5f) * kCell, cz = (y + 0.5f) * kCell;
        if (!t.powered)
          AddSphere(l, { cx - 0.6f, top, cz }, 0.55f, 4, 2, Col(255, 210, 40, MatEmissive), 1, 0.0f);
        if (!t.watered)
          AddSphere(l, { cx + 0.6f, top, cz }, 0.55f, 4, 2, Col(70, 160, 255, MatEmissive), 1, 0.0f);
      }

  for (const City::Vehicle& v : city.vehicles)
  {
    const float dx = static_cast<float>(v.nx - v.x), dz = static_cast<float>(v.ny - v.y);
    const float px = (v.x + 0.5f + dx * v.t) * kCell, pz = (v.y + 0.5f + dz * v.t) * kCell;
    // Right-hand lane: offset perpendicular to travel; opposite directions get opposite lanes.
    const float ox = dz * 1.3f, oz = -dx * 1.3f;
    const float base = city.RoadHeightAt(px + ox, pz + oz) + 0.04f;
    Vec3 dir { dx, 0.0f, dz };
    if (dir.Length() < 0.01f)
      dir = { 1, 0, 0 };
    AddCar(l, { px + ox, base, pz + oz }, dir, v.color, night);
  }

  for (const City::Boat& b : city.boats)
  {
    const float bobY = std::sin(realTime * 1.3f + b.bob) * 0.06f;
    AddBoat(l, { b.x, City::kWaterLevel + 0.15f + bobY, b.z }, b.heading, b.color, realTime * 1.1f + b.bob);
    // Wake: a small steady push behind the stern.
    const float sx = b.x - std::cos(b.heading) * 2.8f, sz = b.z - std::sin(b.heading) * 2.8f;
    AddSplash(sx, sz, 1.6f, 0.035f);
  }

  // Selection: pulsing frame.
  if (City::InBounds(tool.selX, tool.selY))
  {
    const float pulse = 0.5f + 0.5f * std::sin(realTime * 5.0f);
    const uint32_t c = ColF(Vec3 { 1.0f, 0.86f, 0.4f } * (0.7f + 0.3f * pulse), MatUnlit);
    AddTileFrame(l, city, tool.selX, tool.selY, 0.45f, c, 0.55f);
  }

  if (tool.kind == 0 && tool.hoverX < 0)
  {
    l.dirtyMask = kAllSlots;
    return;
  }

  // Tool colours by kind (see Tool in main.cpp).
  auto toolColor = [&](int kind) -> uint32_t {
    switch (kind)
    {
      case 1: case 6: return Col(236, 236, 236, MatUnlit);
      case 2: return Col(120, 230, 100, MatUnlit);
      case 3: return Col(110, 170, 255, MatUnlit);
      case 4: return Col(255, 220, 90, MatUnlit);
      case 5: return Col(240, 70, 60, MatUnlit);
      case 14: return Col(220, 170, 90, MatUnlit);
      case 15: return Col(150, 120, 80, MatUnlit);
      case 16: return Col(200, 200, 120, MatUnlit);
      case 17: return Col(90, 200, 90, MatUnlit);
      default: return kind >= 7 && kind <= 13 ? Col(140, 220, 255, MatUnlit) : Col(255, 255, 255, MatUnlit);
    }
  };
  const uint32_t color = toolColor(tool.kind);
  const bool terrainTool = tool.kind >= 14 && tool.kind <= 16;

  // Faint grid around the cursor while a build tool is active.
  if (tool.kind != 0 && City::InBounds(tool.hoverX, tool.hoverY))
  {
    const uint32_t grid = Col(150, 168, 190, MatUnlit);
    for (int y = tool.hoverY - 4; y <= tool.hoverY + 4; ++y)
      for (int x = tool.hoverX - 4; x <= tool.hoverX + 4; ++x)
        if (City::InBounds(x, y) && (!city.IsWater(x, y) || terrainTool))
          AddTileFrame(l, city, x, y, 0.12f, grid, 0.1f);
  }

  if (tool.kind >= 7 && tool.kind <= 13 && City::InBounds(tool.hoverX, tool.hoverY))
  {
    // Ghost of the service building on the cursor tile.
    const int kind = tool.kind - 7;
    const bool ok = !city.IsWater(tool.hoverX, tool.hoverY) && city.At(tool.hoverX, tool.hoverY).type != City::Road
      && city.At(tool.hoverX, tool.hoverY).type != City::Service;
    std::vector<Light>* sink = m_spotSink;
    m_spotSink = nullptr; // ghosts do not light anything
    const size_t first = l.vertices.size();
    AddServiceModel(l, city, tool.hoverX, tool.hoverY, kind, 0, 1.0f);
    m_spotSink = sink;
    if (!ok)
      for (size_t i = first; i < l.vertices.size(); ++i)
        l.vertices[i].rgba = Col(240, 70, 60, MatUnlit);
    AddTileFrame(l, city, tool.hoverX, tool.hoverY, 0.35f, ok ? color : Col(240, 70, 60, MatUnlit));
  }
  else if ((tool.kind == 14 || tool.kind == 15) && City::InBounds(tool.hoverX, tool.hoverY))
  {
    for (int y = tool.hoverY - tool.brush; y <= tool.hoverY + tool.brush; ++y)
      for (int x = tool.hoverX - tool.brush; x <= tool.hoverX + tool.brush; ++x)
      {
        const float dx = static_cast<float>(x - tool.hoverX), dy = static_cast<float>(y - tool.hoverY);
        if (City::InBounds(x, y) && dx * dx + dy * dy <= (tool.brush + 0.5f) * (tool.brush + 0.5f))
          AddTileFrame(l, city, x, y, city.IsWater(x, y) ? City::kWaterLevel - city.TileTop(x, y) + 0.5f : 0.35f, color, 0.3f);
      }
  }
  else if (tool.dragging)
  {
    if (tool.kind == 1 || tool.kind == 6)
    {
      std::vector<std::pair<int, int>> path;
      City::RoadPath(tool.x0, tool.y0, tool.x1, tool.y1, path);
      for (auto [x, y] : path)
        if (City::InBounds(x, y))
        {
          const bool water = city.IsWater(x, y);
          AddTileFrame(l, city, x, y, water ? City::kBridgeDeck - city.TileTop(x, y) + 0.3f : 0.35f,
            water ? Col(255, 190, 90, MatUnlit) : color);
        }
    }
    else
    {
      for (int y = std::min(tool.y0, tool.y1); y <= std::max(tool.y0, tool.y1); ++y)
        for (int x = std::min(tool.x0, tool.x1); x <= std::max(tool.x0, tool.x1); ++x)
          if (City::InBounds(x, y))
            AddTileFrame(l, city, x, y, city.IsWater(x, y) ? City::kWaterLevel - city.TileTop(x, y) + 0.5f : 0.35f, color);
    }
  }
  else if (City::InBounds(tool.hoverX, tool.hoverY))
  {
    AddTileFrame(l, city, tool.hoverX, tool.hoverY, city.IsWater(tool.hoverX, tool.hoverY) ? City::kWaterLevel - city.TileTop(tool.hoverX, tool.hoverY) + 0.5f : 0.35f, color);
  }

  l.dirtyMask = kAllSlots;
}
