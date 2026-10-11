/*
 * CityAssets.h - the model library loaded from Assets/models/city_assets.glb.
 *
 * The models are authored in Blender (Assets/blender/build_city_assets.py) and
 * exported as one glTF binary, one mesh per asset. Nothing here is textured:
 * each primitive carries a MATERIAL NAME that maps to the shader's material
 * byte (brick, plaster, window, metal, foliage, emissive, ...) and a base
 * colour, which is exactly what the renderer's vertex format already stores.
 * So an authored model drops into the same lighting, shadow, SSS and spotlight
 * pipeline as the procedural geometry it replaces.
 *
 * Missing or unreadable assets are not fatal: Find() returns nullptr and the
 * renderer falls back to its procedural builders, so the sample still runs
 * from a fresh checkout that has never had Blender pointed at it.
 */
#pragma once

#include "CityMath.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class CityAssets
{
public:
  /// Must stay layout-compatible with CityRenderer::Vertex (asserted there).
  struct Vertex
  {
    float px, py, pz;
    float nx, ny, nz;
    uint32_t rgba; ///< R8G8B8A8: rgb from the glTF material, alpha = material id
  };

  struct Model
  {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float height = 0.0f;  ///< highest vertex, for markers and rise animation
    float radius = 0.0f;  ///< footprint radius in the XZ plane
  };

  /// Loads every mesh in the .glb. Returns false when the file is missing or
  /// malformed; the sample then runs on procedural geometry.
  bool Load(const std::string& glbPath);

  /// Model by asset name (the Blender object name, e.g. "res_house").
  /// nullptr when absent.
  const Model* Find(const char* name) const;

  bool IsLoaded() const { return m_loaded; }
  size_t Count() const { return m_models.size(); }
  size_t TriangleCount() const { return m_triangles; }

private:
  std::unordered_map<std::string, Model> m_models;
  size_t m_triangles = 0;
  bool m_loaded = false;
};
