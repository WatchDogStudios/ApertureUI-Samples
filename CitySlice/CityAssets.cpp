/*
 * CityAssets.cpp - glTF model library loading (see CityAssets.h)
 */
#include "CityAssets.h"

#include "CityRenderer.h"

#include <Foundation/Logging/Log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace
{
  /// Blender material name -> the shader's material byte. Anything unknown
  /// shades as plain lit geometry, which is a safe default.
  uint32_t MaterialIdFromName(const char* name)
  {
    if (!name)
      return CityRenderer::MatPlain;
    struct Entry { const char* prefix; uint32_t id; };
    // Longest-prefix wins, so "roof_metal" picks metal-ish roofing, not MatRoof.
    static const Entry kTable[] = {
      { "roof_metal", CityRenderer::MatMetal },
      { "roof_slate", CityRenderer::MatRoof },
      { "roof", CityRenderer::MatRoof },
      { "brick_pale", CityRenderer::MatBrick },
      { "brick", CityRenderer::MatBrick },
      { "plaster_warm", CityRenderer::MatPlaster },
      { "plaster", CityRenderer::MatPlaster },
      { "concrete", CityRenderer::MatConcrete },
      { "glass_dark", CityRenderer::MatWindow },
      { "window", CityRenderer::MatWindow },
      { "metal", CityRenderer::MatMetal },
      { "paint", CityRenderer::MatPaint },
      { "foliage", CityRenderer::MatFoliage },
      { "bark", CityRenderer::MatPlain },
      { "emissive", CityRenderer::MatEmissive },
      { "road", CityRenderer::MatRoad },
      { "grass", CityRenderer::MatGrass },
      { "dirt", CityRenderer::MatPlain },
      { "wood", CityRenderer::MatPlain },
    };
    for (const Entry& e : kTable)
      if (std::strncmp(name, e.prefix, std::strlen(e.prefix)) == 0)
        return e.id;
    return CityRenderer::MatPlain;
  }

  uint32_t PackColor(const float rgba[4], uint32_t materialId)
  {
    auto q = [](float v) {
      // glTF base colours are linear; the renderer's vertex colours are too.
      const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return (materialId << 24) | (q(rgba[2]) << 16) | (q(rgba[1]) << 8) | q(rgba[0]);
  }

  /// Reads a float attribute into `out` (3 components; missing = zero).
  void ReadVec3(const cgltf_accessor* acc, std::vector<float>& out)
  {
    out.assign(acc ? acc->count * 3 : 0, 0.0f);
    if (!acc)
      return;
    for (cgltf_size i = 0; i < acc->count; ++i)
      cgltf_accessor_read_float(acc, i, &out[i * 3], 3);
  }
} // namespace

bool CityAssets::Load(const std::string& glbPath)
{
  cgltf_options options = {};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&options, glbPath.c_str(), &data) != cgltf_result_success)
  {
    nsLog::Warning("CityAssets: no model library at {0} - falling back to procedural geometry",
      glbPath.c_str());
    return false;
  }
  if (cgltf_load_buffers(&options, data, glbPath.c_str()) != cgltf_result_success)
  {
    nsLog::Error("CityAssets: {0} has unreadable buffers", glbPath.c_str());
    cgltf_free(data);
    return false;
  }

  m_models.clear();
  m_triangles = 0;
  for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
  {
    const cgltf_mesh& mesh = data->meshes[mi];
    // Blender exports the object name on the NODE; the mesh usually carries it
    // too, but fall back to the node that references this mesh.
    const char* name = mesh.name;
    if (!name || !*name)
    {
      for (cgltf_size ni = 0; ni < data->nodes_count && (!name || !*name); ++ni)
        if (data->nodes[ni].mesh == &mesh)
          name = data->nodes[ni].name;
    }
    if (!name || !*name)
      continue;

    Model model;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
    {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles)
        continue;

      const cgltf_accessor* posAcc = nullptr;
      const cgltf_accessor* nrmAcc = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
      {
        if (prim.attributes[ai].type == cgltf_attribute_type_position)
          posAcc = prim.attributes[ai].data;
        else if (prim.attributes[ai].type == cgltf_attribute_type_normal)
          nrmAcc = prim.attributes[ai].data;
      }
      if (!posAcc)
        continue;

      float baseColor[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
      const char* matName = prim.material ? prim.material->name : nullptr;
      if (prim.material && prim.material->has_pbr_metallic_roughness)
        std::memcpy(baseColor, prim.material->pbr_metallic_roughness.base_color_factor,
          sizeof(baseColor));
      const uint32_t rgba = PackColor(baseColor, MaterialIdFromName(matName));

      std::vector<float> positions, normals;
      ReadVec3(posAcc, positions);
      ReadVec3(nrmAcc, normals);

      const uint32_t base = static_cast<uint32_t>(model.vertices.size());
      for (cgltf_size vi = 0; vi < posAcc->count; ++vi)
      {
        Vertex v = {};
        v.px = positions[vi * 3 + 0];
        v.py = positions[vi * 3 + 1];
        v.pz = positions[vi * 3 + 2];
        if (!normals.empty())
        {
          v.nx = normals[vi * 3 + 0];
          v.ny = normals[vi * 3 + 1];
          v.nz = normals[vi * 3 + 2];
        }
        else
          v.ny = 1.0f;
        v.rgba = rgba;
        model.vertices.push_back(v);
        model.height = std::max(model.height, v.py);
        model.radius = std::max(model.radius, std::sqrt(v.px * v.px + v.pz * v.pz));
      }

      if (prim.indices)
      {
        for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
          model.indices.push_back(base + static_cast<uint32_t>(
            cgltf_accessor_read_index(prim.indices, ii)));
      }
      else
      {
        for (cgltf_size ii = 0; ii < posAcc->count; ++ii)
          model.indices.push_back(base + static_cast<uint32_t>(ii));
      }
    }
    if (model.indices.empty())
      continue;
    m_triangles += model.indices.size() / 3;
    m_models.emplace(name, std::move(model));
  }
  cgltf_free(data);

  m_loaded = !m_models.empty();
  if (m_loaded)
    nsLog::Info("CityAssets: loaded {0} models ({1} triangles) from {2}",
      m_models.size(), m_triangles, glbPath.c_str());
  else
    nsLog::Warning("CityAssets: {0} contained no usable meshes", glbPath.c_str());
  return m_loaded;
}

const CityAssets::Model* CityAssets::Find(const char* name) const
{
  if (!name)
    return nullptr;
  auto it = m_models.find(name);
  return it == m_models.end() ? nullptr : &it->second;
}
