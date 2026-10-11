/*
 * CityRenderer.cpp - CitySlice world rendering: resources, passes, per-frame
 * orchestration. The procedural meshes live in CityModels.cpp.
 */
#include "CityRenderer.h"

#include "Camera.h"
#include "City.h"
#include "CityRendererInternal.h"

#include <Foundation/Logging/Log.h>

#include <Foundation/Profiling/Profiling.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

static_assert(sizeof(CityAssets::Vertex) == sizeof(float) * 6 + sizeof(uint32_t),
  "CityAssets::Vertex must stay layout-compatible with CityRenderer::Vertex");

void CityRenderer::LoadAssets(const std::string& glbPath)
{
  m_assets.Load(glbPath);
  // The library changes what every layer looks like, so rebuild them all.
  m_terrainBuilt = false;
}

Vec3 CityRenderer::HorizonColor(double day)
{
  return ComputeDayLight(day).horizon;
}

// ============================================================================
// Lifetime
// ============================================================================

bool CityRenderer::Init(applayer::APPlayerVulkanContext& ctx, const std::string& spvDir)
{
  m_ctx = &ctx;
  m_device = ctx.GetDevice();
  m_spvDir = spvDir;

  m_vert = LoadModule(spvDir + "/city.vert.spv");
  m_frag = LoadModule(spvDir + "/city.frag.spv");
  m_postVert = LoadModule(spvDir + "/post.vert.spv");
  m_postFrag = LoadModule(spvDir + "/post.frag.spv");
  m_waveComp = LoadModule(spvDir + "/water.comp.spv");
  if (!m_vert || !m_frag || !m_postVert || !m_postFrag || !m_waveComp)
  {
    nsLog::Error("CityRenderer: shader modules missing under {0}", spvDir.c_str());
    return false;
  }
  if (!CreateLayouts() || !CreateStaticResources())
    return false;
  // The shadow pass is depth-only D32 at 1 sample: its formats never change,
  // so the pipeline is built once here (the scene one follows its HDR target,
  // the post one the swapchain).
  if (!CreatePipeline(VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, true, m_shadowPipeline))
    return false;
  if (!CreatePipeline(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, m_sceneSamples, false, m_pipeline))
    return false;
  if (!CreateComputePipeline())
    return false;
  m_terrainBuilt = false;
  return true;
}

void CityRenderer::Shutdown()
{
  if (m_device == VK_NULL_HANDLE)
    return;
  vkDeviceWaitIdle(m_device);
  DestroyLayer(m_terrain);
  DestroyLayer(m_water);
  DestroyLayer(m_trees);
  DestroyLayer(m_city);
  DestroyLayer(m_buildings);
  DestroyLayer(m_dynamic);
  DestroyLayer(m_overlay);
  DestroyHdrTargets();
  for (uint32_t i = 0; i < kSlots; ++i)
  {
    DestroyBuffer(m_uniforms[i]);
    DestroyBuffer(m_postUniforms[i]);
    DestroyBuffer(m_lightBuf[i]);
    DestroyBuffer(m_gridBuf[i]);
    DestroyBuffer(m_indexBuf[i]);
  }
  DestroyBuffer(m_heightStaging);
  DestroyImage(m_heightmap);
  DestroyImage(m_shadow);
  for (GpuImage& w : m_wave)
    DestroyImage(w);
  auto destroyPipeline = [&](VkPipeline& p) { if (p) vkDestroyPipeline(m_device, p, nullptr); p = VK_NULL_HANDLE; };
  destroyPipeline(m_pipeline);
  destroyPipeline(m_shadowPipeline);
  destroyPipeline(m_postPipeline);
  destroyPipeline(m_wavePipeline);
  if (m_descPool) vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  if (m_linearSampler) vkDestroySampler(m_device, m_linearSampler, nullptr);
  if (m_shadowSampler) vkDestroySampler(m_device, m_shadowSampler, nullptr);
  if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_postPipelineLayout) vkDestroyPipelineLayout(m_device, m_postPipelineLayout, nullptr);
  if (m_computeLayout) vkDestroyPipelineLayout(m_device, m_computeLayout, nullptr);
  if (m_sceneSetLayout) vkDestroyDescriptorSetLayout(m_device, m_sceneSetLayout, nullptr);
  if (m_postSetLayout) vkDestroyDescriptorSetLayout(m_device, m_postSetLayout, nullptr);
  if (m_computeSetLayout) vkDestroyDescriptorSetLayout(m_device, m_computeSetLayout, nullptr);
  for (VkShaderModule* m : { &m_vert, &m_frag, &m_postVert, &m_postFrag, &m_waveComp })
  {
    if (*m) vkDestroyShaderModule(m_device, *m, nullptr);
    *m = VK_NULL_HANDLE;
  }
  m_descPool = VK_NULL_HANDLE;
  m_linearSampler = m_shadowSampler = VK_NULL_HANDLE;
  m_pipelineLayout = m_postPipelineLayout = m_computeLayout = VK_NULL_HANDLE;
  m_sceneSetLayout = m_postSetLayout = m_computeSetLayout = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
  m_ctx = nullptr;
}

VkShaderModule CityRenderer::LoadModule(const std::string& path)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
  {
    nsLog::Error("CityRenderer: cannot open {0}", path.c_str());
    return VK_NULL_HANDLE;
  }
  const std::streamsize size = file.tellg();
  if (size <= 0 || (size % 4) != 0)
    return VK_NULL_HANDLE;
  std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), size);
  if (words[0] != 0x07230203u)
  {
    nsLog::Error("CityRenderer: {0} is not SPIR-V", path.c_str());
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = static_cast<size_t>(size);
  info.pCode = words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  return module;
}

bool CityRenderer::CreateLayouts()
{
  static_assert(sizeof(PushConstants) == 128, "push constants must stay within the portable 128 bytes");
  static_assert(sizeof(SceneUniforms) == 128, "SceneUniforms must match the std140 block in city.slang");
  static_assert(sizeof(Light) == 48, "Light must match the std430 struct in city.slang");
  static_assert(sizeof(PostUniforms) == 240, "PostUniforms must match the std140 block in post.slang");
  static_assert(sizeof(WavePush) <= 128, "wave push constants exceed the portable limit");

  auto makeSetLayout = [&](const std::vector<VkDescriptorSetLayoutBinding>& bindings, VkDescriptorSetLayout& out) {
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    return vkCreateDescriptorSetLayout(m_device, &info, nullptr, &out) == VK_SUCCESS;
  };
  auto makePipelineLayout = [&](VkDescriptorSetLayout set, uint32_t pushBytes, VkShaderStageFlags pushStages,
                              VkPipelineLayout& out) {
    VkPushConstantRange range = { pushStages, 0, pushBytes };
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = pushBytes ? 1 : 0;
    info.pPushConstantRanges = &range;
    return vkCreatePipelineLayout(m_device, &info, nullptr, &out) == VK_SUCCESS;
  };
  const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
  if (!makeSetLayout({
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fs, nullptr },   // shadow map
        { 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, fs, nullptr },         // comparison sampler
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fs, nullptr },  // lights
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fs, nullptr },  // light grid
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fs, nullptr },  // light refs
        { 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, vf, nullptr },   // wave heights
        { 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, vf, nullptr },   // terrain heights
        { 8, VK_DESCRIPTOR_TYPE_SAMPLER, 1, vf, nullptr },         // linear sampler
      }, m_sceneSetLayout))
    return false;
  if (!makeSetLayout({
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, fs, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fs, nullptr }, // hdr colour
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fs, nullptr }, // normal / roughness
        { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fs, nullptr }, // depth
        { 4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, fs, nullptr },
      }, m_postSetLayout))
    return false;
  if (!makeSetLayout({
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr }, // previous
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr }, // current
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr }, // next
        { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, cs, nullptr }, // terrain heights
        { 4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, cs, nullptr },
      }, m_computeSetLayout))
    return false;
  if (!makePipelineLayout(m_sceneSetLayout, sizeof(PushConstants), vf, m_pipelineLayout)
    || !makePipelineLayout(m_postSetLayout, 0, 0, m_postPipelineLayout)
    || !makePipelineLayout(m_computeSetLayout, sizeof(WavePush), cs, m_computeLayout))
  {
    nsLog::Error("CityRenderer: pipeline layout creation failed");
    return false;
  }
  return true;
}

bool CityRenderer::CreateImage(GpuImage& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
  VkSampleCountFlagBits samples, VkImageAspectFlags aspect)
{
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = { w, h, 1 };
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(m_device, &info, nullptr, &img.image) != VK_SUCCESS)
  {
    nsLog::Error("CityRenderer: vkCreateImage failed ({0}x{1} fmt {2})", w, h, static_cast<int>(format));
    return false;
  }
  VkMemoryRequirements req = {};
  vkGetImageMemoryRequirements(m_device, img.image, &req);
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = m_ctx->FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(m_device, &alloc, nullptr, &img.memory) != VK_SUCCESS)
  {
    nsLog::Error("CityRenderer: image memory allocation failed");
    return false;
  }
  vkBindImageMemory(m_device, img.image, img.memory, 0);
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = img.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = { aspect, 0, 1, 0, 1 };
  if (vkCreateImageView(m_device, &view, nullptr, &img.view) != VK_SUCCESS)
    return false;
  img.format = format;
  img.width = w;
  img.height = h;
  return true;
}

void CityRenderer::DestroyImage(GpuImage& img)
{
  if (img.view) vkDestroyImageView(m_device, img.view, nullptr);
  if (img.image) vkDestroyImage(m_device, img.image, nullptr);
  if (img.memory) vkFreeMemory(m_device, img.memory, nullptr);
  img = GpuImage {};
}

bool CityRenderer::CreateStaticResources()
{
  // ---- shadow map, wave images, heightmap ----------------------------------
  if (!CreateImage(m_shadow, kShadowRes, kShadowRes, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT))
    return false;
  for (GpuImage& w : m_wave)
    if (!CreateImage(w, kWaveRes, kWaveRes, VK_FORMAT_R32_SFLOAT,
          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
      return false;
  if (!CreateImage(m_heightmap, City::N + 1, City::N + 1, VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT))
    return false;
  if (!EnsureBuffer(m_heightStaging, (City::N + 1) * (City::N + 1) * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
    return false;

  // ---- samplers -------------------------------------------------------------
  VkSamplerCreateInfo samp = {};
  samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samp.magFilter = samp.minFilter = VK_FILTER_LINEAR;
  samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.maxLod = 1.0f;
  if (vkCreateSampler(m_device, &samp, nullptr, &m_linearSampler) != VK_SUCCESS)
    return false;
  // Hardware PCF: linear filtering on a comparison sampler; outside the map
  // the white border reads as "lit".
  samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  samp.compareEnable = VK_TRUE;
  samp.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  if (vkCreateSampler(m_device, &samp, nullptr, &m_shadowSampler) != VK_SUCCESS)
    return false;

  // ---- descriptor pool + sets ------------------------------------------------
  const VkDescriptorPoolSize sizes[] = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSlots },
    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 * kSlots + 3 },
    { VK_DESCRIPTOR_TYPE_SAMPLER, 3 * kSlots + 3 },
    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSlots },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9 },
  };
  VkDescriptorPoolCreateInfo pool = {};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 2 * kSlots + 3;
  pool.poolSizeCount = static_cast<uint32_t>(sizeof(sizes) / sizeof(sizes[0]));
  pool.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descPool) != VK_SUCCESS)
    return false;
  auto allocSets = [&](VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet* out) {
    std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_descPool;
    info.descriptorSetCount = count;
    info.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(m_device, &info, out) == VK_SUCCESS;
  };
  if (!allocSets(m_sceneSetLayout, kSlots, m_sceneSets) || !allocSets(m_postSetLayout, kSlots, m_postSets)
    || !allocSets(m_computeSetLayout, 3, m_waveSets))
  {
    nsLog::Error("CityRenderer: descriptor set allocation failed");
    return false;
  }

  for (uint32_t i = 0; i < kSlots; ++i)
  {
    if (!EnsureBuffer(m_uniforms[i], sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      || !EnsureBuffer(m_postUniforms[i], sizeof(PostUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      || !EnsureBuffer(m_lightBuf[i], kMaxLights * sizeof(Light), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      || !EnsureBuffer(m_gridBuf[i], City::N * City::N * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      || !EnsureBuffer(m_indexBuf[i], kMaxLightRefs * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
      return false;
    std::memset(m_uniforms[i].mapped, 0, sizeof(SceneUniforms));
    std::memset(m_gridBuf[i].mapped, 0, City::N * City::N * sizeof(uint32_t));
    WriteSceneSet(i);
  }

  // Wave sets: rotation r reads r-1 (prev) and r (cur), writes r+1 (next).
  for (int r = 0; r < 3; ++r)
  {
    const VkDescriptorImageInfo prev = { VK_NULL_HANDLE, m_wave[(r + 2) % 3].view, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo cur = { VK_NULL_HANDLE, m_wave[r].view, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo next = { VK_NULL_HANDLE, m_wave[(r + 1) % 3].view, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo hm = { VK_NULL_HANDLE, m_heightmap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo sampler = { m_linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w[5] = {};
    const VkDescriptorImageInfo* infos[5] = { &prev, &cur, &next, &hm, &sampler };
    const VkDescriptorType types[5] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER };
    for (int b = 0; b < 5; ++b)
    {
      w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[b].dstSet = m_waveSets[r];
      w[b].dstBinding = static_cast<uint32_t>(b);
      w[b].descriptorCount = 1;
      w[b].descriptorType = types[b];
      w[b].pImageInfo = infos[b];
    }
    vkUpdateDescriptorSets(m_device, 5, w, 0, nullptr);
  }
  return true;
}

void CityRenderer::WriteSceneSet(uint32_t slot)
{
  const VkDescriptorBufferInfo ubo = { m_uniforms[slot].buffer, 0, sizeof(SceneUniforms) };
  const VkDescriptorImageInfo shadow = { VK_NULL_HANDLE, m_shadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
  const VkDescriptorImageInfo shadowSamp = { m_shadowSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
  const VkDescriptorBufferInfo lights = { m_lightBuf[slot].buffer, 0, VK_WHOLE_SIZE };
  const VkDescriptorBufferInfo grid = { m_gridBuf[slot].buffer, 0, VK_WHOLE_SIZE };
  const VkDescriptorBufferInfo refs = { m_indexBuf[slot].buffer, 0, VK_WHOLE_SIZE };
  const VkDescriptorImageInfo wave = { VK_NULL_HANDLE, m_wave[m_waveCur].view, VK_IMAGE_LAYOUT_GENERAL };
  const VkDescriptorImageInfo hm = { VK_NULL_HANDLE, m_heightmap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  const VkDescriptorImageInfo linear = { m_linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
  VkWriteDescriptorSet w[9] = {};
  for (int b = 0; b < 9; ++b)
  {
    w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[b].dstSet = m_sceneSets[slot];
    w[b].dstBinding = static_cast<uint32_t>(b);
    w[b].descriptorCount = 1;
  }
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[1].pImageInfo = &shadow;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[2].pImageInfo = &shadowSamp;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &lights;
  w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &grid;
  w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &refs;
  w[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[6].pImageInfo = &wave;
  w[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[7].pImageInfo = &hm;
  w[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[8].pImageInfo = &linear;
  vkUpdateDescriptorSets(m_device, 9, w, 0, nullptr);
}

void CityRenderer::WritePostSets()
{
  for (uint32_t slot = 0; slot < kSlots; ++slot)
  {
    const VkDescriptorBufferInfo ubo = { m_postUniforms[slot].buffer, 0, sizeof(PostUniforms) };
    const VkDescriptorImageInfo hdr = { VK_NULL_HANDLE, m_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo aux = { VK_NULL_HANDLE, m_aux.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo depth = { VK_NULL_HANDLE, m_depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo linear = { m_linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w[5] = {};
    for (int b = 0; b < 5; ++b)
    {
      w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[b].dstSet = m_postSets[slot];
      w[b].dstBinding = static_cast<uint32_t>(b);
      w[b].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[1].pImageInfo = &hdr;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[2].pImageInfo = &aux;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[3].pImageInfo = &depth;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[4].pImageInfo = &linear;
    vkUpdateDescriptorSets(m_device, 5, w, 0, nullptr);
  }
}

bool CityRenderer::CreateHdrTargets(uint32_t width, uint32_t height)
{
  vkDeviceWaitIdle(m_device);
  DestroyHdrTargets();
  const VkFormat cf = VK_FORMAT_R16G16B16A16_SFLOAT, df = VK_FORMAT_D32_SFLOAT;
  const VkImageUsageFlags colorMs = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  const VkImageUsageFlags colorRes = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags depthMs = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  const VkImageUsageFlags depthRes = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!CreateImage(m_hdrMsaa, width, height, cf, colorMs, m_sceneSamples, VK_IMAGE_ASPECT_COLOR_BIT)
    || !CreateImage(m_hdr, width, height, cf, colorRes, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)
    || !CreateImage(m_auxMsaa, width, height, cf, colorMs, m_sceneSamples, VK_IMAGE_ASPECT_COLOR_BIT)
    || !CreateImage(m_aux, width, height, cf, colorRes, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)
    || !CreateImage(m_depthMsaa, width, height, df, depthMs, m_sceneSamples, VK_IMAGE_ASPECT_DEPTH_BIT)
    || !CreateImage(m_depth, width, height, df, depthRes, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
  {
    nsLog::Error("CityRenderer: HDR target creation failed ({0}x{1})", width, height);
    DestroyHdrTargets();
    return false;
  }
  m_hdrWidth = width;
  m_hdrHeight = height;
  m_hdrValid = true;
  WritePostSets();
  return true;
}

void CityRenderer::DestroyHdrTargets()
{
  for (GpuImage* img : { &m_hdrMsaa, &m_hdr, &m_auxMsaa, &m_aux, &m_depthMsaa, &m_depth })
    DestroyImage(*img);
  m_hdrWidth = m_hdrHeight = 0;
  m_hdrValid = false;
}

bool CityRenderer::CreatePipeline(VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples,
  bool shadow, VkPipeline& out)
{
  if (out != VK_NULL_HANDLE)
  {
    vkDeviceWaitIdle(m_device);
    vkDestroyPipeline(m_device, out, nullptr);
    out = VK_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = m_vert;
  stages[0].pName = "main"; // slangc emits every entry point as "main"
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = m_frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding = {};
  binding.binding = 0;
  binding.stride = sizeof(Vertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[3] = {};
  attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px) };
  attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx) };
  attrs[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, rgba) };
  VkPipelineVertexInputStateCreateInfo vertexInput = {};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 3;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo assembly = {};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // ponytail: no culling; low-poly geometry, and it removes a whole class of
  // winding bugs. Flip to BACK if overdraw ever shows up in a profile.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  if (shadow)
  {
    // Push the stored depth away from the light; the shader adds a normal
    // offset on top, together they hide acne without visible peter-panning.
    raster.depthBiasEnable = VK_TRUE;
    raster.depthBiasConstantFactor = 2.0f;
    raster.depthBiasSlopeFactor = 2.5f;
  }

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = samples;

  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttach[2] = {};
  for (auto& b : blendAttach)
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = shadow ? 0 : 2;
  blend.pAttachments = blendAttach;

  const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;

  const VkFormat colorFormats[2] = { colorFormat, colorFormat };
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = shadow ? 0 : 2;
  rendering.pColorAttachmentFormats = colorFormats;
  rendering.depthAttachmentFormat = depthFormat;

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = shadow ? 1 : 2; // depth-only: no fragment stage
  info.pStages = stages;
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = m_pipelineLayout;
  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &out) != VK_SUCCESS)
  {
    nsLog::Error("CityRenderer: vkCreateGraphicsPipelines failed ({0})", shadow ? "shadow" : "scene");
    return false;
  }
  return true;
}

bool CityRenderer::CreatePostPipeline(VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples)
{
  if (m_postPipeline != VK_NULL_HANDLE)
  {
    vkDeviceWaitIdle(m_device);
    vkDestroyPipeline(m_device, m_postPipeline, nullptr);
    m_postPipeline = VK_NULL_HANDLE;
  }
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = m_postVert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = m_postFrag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput = {};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // fullscreen triangle from SV_VertexID
  VkPipelineInputAssemblyStateCreateInfo assembly = {};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = samples;
  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; // test + write off: the UI draws over us
  VkPipelineColorBlendAttachmentState blendAttach = {};
  blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blendAttach;
  const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;
  const bool depthHasStencil = depthFormat != VK_FORMAT_D32_SFLOAT && depthFormat != VK_FORMAT_D16_UNORM;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &colorFormat;
  rendering.depthAttachmentFormat = depthFormat;
  rendering.stencilAttachmentFormat = depthHasStencil ? depthFormat : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = m_postPipelineLayout;
  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_postPipeline) != VK_SUCCESS)
  {
    nsLog::Error("CityRenderer: post pipeline creation failed");
    return false;
  }
  m_postSamples = samples;
  m_postColorFormat = colorFormat;
  m_postDepthFormat = depthFormat;
  return true;
}

bool CityRenderer::CreateComputePipeline()
{
  VkComputePipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = m_waveComp;
  info.stage.pName = "main";
  info.layout = m_computeLayout;
  if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_wavePipeline) != VK_SUCCESS)
  {
    nsLog::Error("CityRenderer: wave compute pipeline creation failed");
    return false;
  }
  return true;
}

// ============================================================================
// Buffers
// ============================================================================

bool CityRenderer::EnsureBuffer(GpuBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage)
{
  if (buf.buffer != VK_NULL_HANDLE && buf.capacity >= size)
    return true;
  // Callers only touch a slot whose fence BeginFrame already waited on, so the
  // old buffer is not in flight and can go straight away.
  DestroyBuffer(buf);
  VkDeviceSize capacity = std::max<VkDeviceSize>(size + size / 2, 64 * 1024);
  VkBufferCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = capacity;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(m_device, &info, nullptr, &buf.buffer) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req = {};
  vkGetBufferMemoryRequirements(m_device, buf.buffer, &req);
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = m_ctx->FindMemoryTypeIndex(req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(m_device, &alloc, nullptr, &buf.memory) != VK_SUCCESS)
  {
    DestroyBuffer(buf);
    return false;
  }
  vkBindBufferMemory(m_device, buf.buffer, buf.memory, 0);
  if (vkMapMemory(m_device, buf.memory, 0, capacity, 0, &buf.mapped) != VK_SUCCESS)
  {
    DestroyBuffer(buf);
    return false;
  }
  buf.capacity = capacity;
  return true;
}

void CityRenderer::DestroyBuffer(GpuBuffer& buf)
{
  if (buf.mapped) vkUnmapMemory(m_device, buf.memory);
  if (buf.buffer) vkDestroyBuffer(m_device, buf.buffer, nullptr);
  if (buf.memory) vkFreeMemory(m_device, buf.memory, nullptr);
  buf = GpuBuffer {};
}

void CityRenderer::DestroyLayer(Layer& layer)
{
  for (uint32_t s = 0; s < kSlots; ++s)
  {
    DestroyBuffer(layer.vb[s]);
    DestroyBuffer(layer.ib[s]);
    layer.uploadedIndices[s] = 0;
  }
  layer.dirtyMask = kAllSlots;
}

void CityRenderer::UploadLayer(Layer& layer, uint32_t slot)
{
  if (!(layer.dirtyMask & (1u << slot)))
    return;
  layer.dirtyMask &= ~(1u << slot);
  layer.uploadedIndices[slot] = 0;
  if (layer.indices.empty())
    return;
  const VkDeviceSize vbytes = layer.vertices.size() * sizeof(Vertex);
  const VkDeviceSize ibytes = layer.indices.size() * sizeof(uint32_t);
  if (!EnsureBuffer(layer.vb[slot], vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
    || !EnsureBuffer(layer.ib[slot], ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
  {
    nsLog::Error("CityRenderer: buffer allocation failed ({0} verts)", layer.vertices.size());
    return;
  }
  std::memcpy(layer.vb[slot].mapped, layer.vertices.data(), static_cast<size_t>(vbytes));
  std::memcpy(layer.ib[slot].mapped, layer.indices.data(), static_cast<size_t>(ibytes));
  layer.uploadedIndices[slot] = static_cast<uint32_t>(layer.indices.size());
}

void CityRenderer::DrawLayer(VkCommandBuffer cmd, Layer& layer, uint32_t slot)
{
  if (layer.uploadedIndices[slot] == 0)
    return;
  const VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &layer.vb[slot].buffer, &zero);
  vkCmdBindIndexBuffer(cmd, layer.ib[slot].buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(cmd, layer.uploadedIndices[slot], 1, 0, 0, 0);
}

void CityRenderer::ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
  VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
  VkAccessFlags dstAccess)
{
  VkImageMemoryBarrier b = {};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = { aspect, 0, 1, 0, 1 };
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ============================================================================
// Frame
// ============================================================================

void CityRenderer::AddSplash(float x, float z, float radius, float strength)
{
  if (m_impulses.size() >= 64 * 4)
    return;
  const float extent = City::N * City::kCell;
  m_impulses.push_back(x / extent);
  m_impulses.push_back(z / extent);
  m_impulses.push_back(radius / extent);
  m_impulses.push_back(strength);
}

void CityRenderer::AddSpot(Vec3 pos, Vec3 dir, Vec3 color, float intensity, float range, float innerDeg, float outerDeg)
{
  if (!m_spotSink || m_spotSink->size() >= kMaxLights)
    return;
  const Vec3 d = dir.Normalized();
  Light l;
  l.px = pos.x; l.py = pos.y; l.pz = pos.z; l.range = range;
  l.dx = d.x; l.dy = d.y; l.dz = d.z;
  l.cosOuter = std::cos(outerDeg * kPi / 180.0f);
  l.r = color.x * intensity; l.g = color.y * intensity; l.b = color.z * intensity;
  l.cosInner = std::cos(innerDeg * kPi / 180.0f);
  m_spotSink->push_back(l);
}

void CityRenderer::BuildLightGrid(double day)
{
  const DayLight L = ComputeDayLight(day);
  const float lightsOn = Clamp((L.night - 0.1f) * 2.5f, 0.0f, 1.0f);
  std::vector<Light>& all = m_frameLights; // dynamic lights (cars, boats) already collected
  if (lightsOn > 0.0f)
  {
    all.insert(all.end(), m_lampLights.begin(), m_lampLights.end());
    all.insert(all.end(), m_buildingLights.begin(), m_buildingLights.end());
  }
  else
    all.clear();
  if (all.size() > kMaxLights)
    all.resize(kMaxLights);
  for (Light& l : all)
  {
    l.r *= lightsOn;
    l.g *= lightsOn;
    l.b *= lightsOn;
  }

  // Tile buckets: offset<<8 | count per tile, then a flat reference list.
  const int n = City::N;
  std::vector<uint32_t> counts(static_cast<size_t>(n) * n, 0);
  struct Ref { int tile; uint32_t light; };
  std::vector<Ref> refs;
  refs.reserve(all.size() * 9);
  for (uint32_t i = 0; i < all.size(); ++i)
  {
    const Light& l = all[i];
    const int x0 = std::max(0, static_cast<int>((l.px - l.range) / City::kCell));
    const int x1 = std::min(n - 1, static_cast<int>((l.px + l.range) / City::kCell));
    const int z0 = std::max(0, static_cast<int>((l.pz - l.range) / City::kCell));
    const int z1 = std::min(n - 1, static_cast<int>((l.pz + l.range) / City::kCell));
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x)
      {
        const int tile = z * n + x;
        if (counts[tile] >= 16)
          continue;
        ++counts[tile];
        refs.push_back({ tile, i });
      }
  }
  m_grid.assign(static_cast<size_t>(n) * n, 0);
  uint32_t offset = 0;
  for (int t = 0; t < n * n; ++t)
  {
    m_grid[t] = (offset << 8) | counts[t];
    offset += counts[t];
    counts[t] = 0;
  }
  m_lightRefs.assign(std::max<size_t>(1, std::min<size_t>(offset, kMaxLightRefs)), 0);
  for (const Ref& r : refs)
  {
    const uint32_t slotIdx = (m_grid[r.tile] >> 8) + counts[r.tile]++;
    if (slotIdx < m_lightRefs.size())
      m_lightRefs[slotIdx] = r.light;
  }
}

void CityRenderer::UploadLights(uint32_t slot)
{
  if (!m_frameLights.empty())
    std::memcpy(m_lightBuf[slot].mapped, m_frameLights.data(), m_frameLights.size() * sizeof(Light));
  std::memcpy(m_gridBuf[slot].mapped, m_grid.data(), m_grid.size() * sizeof(uint32_t));
  std::memcpy(m_indexBuf[slot].mapped, m_lightRefs.data(), m_lightRefs.size() * sizeof(uint32_t));
}

void CityRenderer::BuildCpu(City& city, const Camera& camera, const ToolPreview& tool, float realTime)
{
  NS_PROFILE_SCOPE("City::BuildCpu");
  m_frameLights.clear();
  if (city.terrainDirty || !m_terrainBuilt)
  {
    BuildTerrain(city);
    BuildTrees(city);
    PruneTrees(city);
    m_heightmapDirty = true;
    city.terrainDirty = false;
    m_terrainBuilt = true;
  }
  if (!m_waterBuilt)
  {
    BuildWater();
    m_waterBuilt = true;
  }
  // Roads/lots change only when tiles change (player actions); trees are
  // pruned in place. Buildings rebuild on their own flag, and the ones still
  // rising live in the per-frame layer so a spawn never regenerates the rest.
  const bool rising = city.gameSeconds < city.animatingUntil;
  if (city.geometryDirty)
  {
    m_lampLights.clear();
    m_spotSink = &m_lampLights;
    BuildCity(city);
    PruneTrees(city);
    city.geometryDirty = false;
    city.buildingsDirty = true;
  }
  if (city.buildingsDirty || (m_wasRising && !rising))
  {
    m_buildingLights.clear();
    m_spotSink = &m_buildingLights;
    BuildBuildings(city, rising);
    city.buildingsDirty = false;
  }
  m_wasRising = rising;
  m_spotSink = &m_frameLights;
  BuildDynamic(city, camera, tool, realTime);
  if (tool.overlay != m_overlayKind || city.coverageVersion != m_overlayVersion)
  {
    BuildOverlay(city, tool);
    m_overlayKind = tool.overlay;
    m_overlayVersion = city.coverageVersion;
  }
  BuildLightGrid(city.day);
  for (const City::Splash& s : city.splashes)
    AddSplash(s.x, s.z, s.radius, s.strength);
  city.splashes.clear();
  m_cpuBuilt = true;
}

void CityRenderer::UploadAll(uint32_t slot)
{
  NS_PROFILE_SCOPE("City::Upload");
  UploadLights(slot);
  UploadLayer(m_terrain, slot);
  UploadLayer(m_water, slot);
  UploadLayer(m_trees, slot);
  UploadLayer(m_city, slot);
  UploadLayer(m_buildings, slot);
  UploadLayer(m_dynamic, slot);
  UploadLayer(m_overlay, slot);
}

void CityRenderer::FillPushConstants(PushConstants& pc, const Mat4& viewProj, const Camera& camera,
  double day, float realTime) const
{
  const Vec3 eye = camera.Eye();
  const DayLight L = ComputeDayLight(day);
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      pc.col[c][r] = viewProj.m[c * 4 + r];
  pc.sun[0] = L.sunDir.x; pc.sun[1] = L.sunDir.y; pc.sun[2] = L.sunDir.z; pc.sun[3] = L.night;
  pc.cam[0] = eye.x; pc.cam[1] = eye.y; pc.cam[2] = eye.z; pc.cam[3] = realTime;
  pc.fog[0] = L.horizon.x; pc.fog[1] = L.horizon.y; pc.fog[2] = L.horizon.z;
  pc.fog[3] = 1100.0f + camera.sDist * 2.5f;
  pc.sunColor[0] = L.sunColor.x; pc.sunColor[1] = L.sunColor.y; pc.sunColor[2] = L.sunColor.z;
  pc.sunColor[3] = L.ambient;
}

/// Orthographic sun frustum around the ground the camera looks at, snapped to
/// shadow texels so the edges stay put while the camera moves.
Mat4 CityRenderer::ComputeLightViewProj(const Camera& camera, Vec3 sunDir) const
{
  const Vec3 f = camera.Forward();
  const Vec3 fwd = Vec3 { f.x, 0.0f, f.z }.Normalized();
  const float extent = Clamp(camera.sDist * 1.5f, 80.0f, 560.0f);
  const Vec3 centre = camera.sTarget + fwd * (camera.sDist * 0.25f);
  // sunDir always has a z component, so a fixed world up never degenerates.
  const Mat4 view = Mat4::LookAt(sunDir * 800.0f, Vec3 { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
  float cx = view.m[0] * centre.x + view.m[4] * centre.y + view.m[8] * centre.z + view.m[12];
  float cy = view.m[1] * centre.x + view.m[5] * centre.y + view.m[9] * centre.z + view.m[13];
  const float cz = view.m[2] * centre.x + view.m[6] * centre.y + view.m[10] * centre.z + view.m[14];
  const float texel = 2.0f * extent / kShadowRes;
  cx = std::floor(cx / texel) * texel;
  cy = std::floor(cy / texel) * texel;
  const float zNear = -cz - 900.0f, zFar = -cz + 900.0f;
  Mat4 ortho;
  ortho.m[0] = 1.0f / extent;
  ortho.m[5] = -1.0f / extent; // Vulkan clip Y down, like Mat4::Perspective
  ortho.m[10] = -1.0f / (zFar - zNear);
  ortho.m[12] = -cx / extent;
  ortho.m[13] = cy / extent;
  ortho.m[14] = -zNear / (zFar - zNear);
  ortho.m[15] = 1.0f;
  return ortho * view;
}

void CityRenderer::RecordHeightmapUpload(VkCommandBuffer cmd, const City& city)
{
  const std::vector<float>& h = city.Heights();
  std::memcpy(m_heightStaging.mapped, h.data(), h.size() * sizeof(float));
  ImageBarrier(cmd, m_heightmap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy region = {};
  region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  region.imageExtent = { City::N + 1, City::N + 1, 1 };
  vkCmdCopyBufferToImage(cmd, m_heightStaging.buffer, m_heightmap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ImageBarrier(cmd, m_heightmap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT);
  m_heightmapDirty = false;
  m_heightmapReady = true;
}

void CityRenderer::RecordWaveStep(VkCommandBuffer cmd, float dt)
{
  NS_PROFILE_SCOPE("City::WaveSim");
  if (!m_waveInitialised)
  {
    // Clear all three height buffers and park them in GENERAL for good.
    for (GpuImage& w : m_wave)
    {
      ImageBarrier(cmd, w.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      const VkClearColorValue zero = {};
      const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cmd, w.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
      ImageBarrier(cmd, w.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    m_waveInitialised = true;
  }
  // Fixed 60 Hz substeps keep the wave equation stable at any frame rate.
  m_waveAccum += std::min(dt, 0.1f);
  int steps = static_cast<int>(m_waveAccum * 60.0f);
  steps = std::min(steps, 3);
  m_waveAccum -= steps / 60.0f;
  if (steps == 0 && !m_impulses.empty())
    steps = 1;
  VkMemoryBarrier mem = {};
  mem.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  for (int s = 0; s < steps; ++s)
  {
    mem.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mem.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mem, 0, nullptr, 0, nullptr);
    WavePush push = {};
    int count = 0;
    while (count < kMaxImpulses && !m_impulses.empty())
    {
      const size_t base = m_impulses.size() - 4;
      for (int k = 0; k < 4; ++k)
        push.impulses[count][k] = m_impulses[base + k];
      m_impulses.resize(base);
      ++count;
    }
    push.params[0] = 0.994f;             // damping
    push.params[1] = 0.42f;              // c^2 (stable below 0.5)
    push.params[2] = City::kWaterLevel;
    push.params[3] = static_cast<float>(count);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_wavePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computeLayout, 0, 1, &m_waveSets[m_waveCur], 0, nullptr);
    vkCmdPushConstants(cmd, m_computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, kWaveRes / 16, kWaveRes / 16, 1);
    m_waveCur = (m_waveCur + 1) % 3;
  }
  mem.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mem.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mem, 0, nullptr, 0, nullptr);
}

void CityRenderer::RecordShadowPass(VkCommandBuffer cmd, const Camera& camera, double day, float realTime, uint32_t slot)
{
  NS_PROFILE_SCOPE("City::ShadowPass");
  // Last frame's scene pass may still be sampling the map: order the write after it.
  ImageBarrier(cmd, m_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth.imageView = m_shadow.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil = { 1.0f, 0 };
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea = { { 0, 0 }, { kShadowRes, kShadowRes } };
  info.layerCount = 1;
  info.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &info);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
  VkViewport vp = {};
  vp.width = vp.height = static_cast<float>(kShadowRes);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  const VkRect2D scissor = { { 0, 0 }, { kShadowRes, kShadowRes } };
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_sceneSets[slot], 0, nullptr);
  PushConstants pc = {};
  FillPushConstants(pc, m_lightViewProj, camera, day, realTime);
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  // Static casters only: the dynamic layer holds the sky dome (which would
  // blanket the map), cars, boats and rising buildings.
  DrawLayer(cmd, m_terrain, slot);
  DrawLayer(cmd, m_trees, slot);
  DrawLayer(cmd, m_city, slot);
  DrawLayer(cmd, m_buildings, slot);
  vkCmdEndRendering(cmd);
  ImageBarrier(cmd, m_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  m_shadowValid = true;
}

void CityRenderer::RecordScenePass(VkCommandBuffer cmd, const Camera& camera, double day, float realTime, uint32_t slot)
{
  NS_PROFILE_SCOPE("City::ScenePass");
  // Attachments: the previous frame's post pass read the resolved images.
  const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkPipelineStageFlags depthStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  for (GpuImage* img : { &m_hdrMsaa, &m_hdr, &m_auxMsaa, &m_aux })
    ImageBarrier(cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  for (GpuImage* img : { &m_depthMsaa, &m_depth })
    ImageBarrier(cmd, img->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, depthStage, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  const DayLight L = ComputeDayLight(day);
  VkRenderingAttachmentInfo color[2] = {};
  for (int i = 0; i < 2; ++i)
  {
    color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color[i].imageView = i == 0 ? m_hdrMsaa.view : m_auxMsaa.view;
    color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color[i].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color[i].resolveImageView = i == 0 ? m_hdr.view : m_aux.view;
    color[i].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  color[0].clearValue.color = { { L.horizon.x, L.horizon.y, L.horizon.z, 1.0f } };
  color[1].clearValue.color = { { 0.5f, 1.0f, 0.5f, -1.0f } };
  VkRenderingAttachmentInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth.imageView = m_depthMsaa.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
  depth.resolveImageView = m_depth.view;
  depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = { 1.0f, 0 };
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea = { { 0, 0 }, { m_hdrWidth, m_hdrHeight } };
  info.layerCount = 1;
  info.colorAttachmentCount = 2;
  info.pColorAttachments = color;
  info.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &info);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_sceneSets[slot], 0, nullptr);
  VkViewport vp = {};
  vp.width = static_cast<float>(m_hdrWidth);
  vp.height = static_cast<float>(m_hdrHeight);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  const VkRect2D scissor = { { 0, 0 }, { m_hdrWidth, m_hdrHeight } };
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const float aspect = vp.height > 0.0f ? vp.width / vp.height : 1.0f;
  const Mat4 viewProj = camera.Proj(aspect) * camera.View();
  PushConstants pc = {};
  FillPushConstants(pc, viewProj, camera, day, realTime);
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

  DrawLayer(cmd, m_dynamic, slot); // sky dome first (far), then everything sorts by depth
  DrawLayer(cmd, m_terrain, slot);
  DrawLayer(cmd, m_trees, slot);
  DrawLayer(cmd, m_city, slot);
  DrawLayer(cmd, m_buildings, slot);
  DrawLayer(cmd, m_overlay, slot);
  DrawLayer(cmd, m_water, slot);
  vkCmdEndRendering(cmd);

  for (GpuImage* img : { &m_hdr, &m_aux })
    ImageBarrier(cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  ImageBarrier(cmd, m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthStage, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void CityRenderer::Prepare(const Frame& frame, City& city, const Camera& camera,
  const ToolPreview& tool, float realTime)
{
  NS_PROFILE_SCOPE("City::Prepare");
  static float s_lastTime = -1.0f;
  const float dt = s_lastTime < 0.0f ? 1.0f / 60.0f : Clamp(realTime - s_lastTime, 0.0f, 0.1f);
  s_lastTime = realTime;
  const uint32_t slot = frame.frameIndex % kSlots;
  if (!m_hdrValid || m_hdrWidth != frame.extent.width || m_hdrHeight != frame.extent.height)
    if (!CreateHdrTargets(frame.extent.width, frame.extent.height))
      return;

  static const bool s_prof = std::getenv("CITYSLICE_PROF") != nullptr;
  static double s_acc[5] = {};
  static int s_frames = 0;
  auto lap = [](std::chrono::steady_clock::time_point& t) {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - t).count();
    t = now;
    return ms;
  };
  auto t = std::chrono::steady_clock::now();
  if (!m_cpuBuilt)
    BuildCpu(city, camera, tool, realTime);
  UploadAll(slot);
  m_cpuBuilt = false;
  s_acc[0] += lap(t);
  m_preparedFrame = frame.frameIndex;
  m_camEye = camera.Eye();
  VkCommandBuffer cmd = frame.cmd;
  if (m_heightmapDirty)
    RecordHeightmapUpload(cmd, city);
  RecordWaveStep(cmd, dt);
  s_acc[1] += lap(t);

  // Per-frame uniforms (the wave binding follows the rotation).
  const DayLight L = ComputeDayLight(city.day);
  m_lightViewProj = ComputeLightViewProj(camera, L.sunDir);
  SceneUniforms u = {};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      u.light[c][r] = m_lightViewProj.m[c * 4 + r];
  u.shadow[0] = Lerp(0.92f, 0.6f, L.night);
  u.shadow[1] = 1.0f / kShadowRes;
  u.shadow[2] = 0.6f * (2.0f * Clamp(camera.sDist * 1.5f, 80.0f, 560.0f) / kShadowRes); // ~0.6 texel
  const float extent = City::N * City::kCell;
  u.water[0] = City::kWaterLevel;
  u.water[1] = 1.0f / extent;
  u.water[2] = 0.32f;
  u.water[3] = 1.0f / kWaveRes;
  u.zenith[0] = L.zenith.x; u.zenith[1] = L.zenith.y; u.zenith[2] = L.zenith.z;
  u.zenith[3] = Clamp((L.night - 0.1f) * 2.5f, 0.0f, 1.0f);
  u.terrain[0] = 1.0f / (City::N + 1);
  u.terrain[1] = 0.5f / (City::N + 1);
  u.terrain[2] = City::kCell;
  u.terrain[3] = static_cast<float>(City::N);
  std::memcpy(m_uniforms[slot].mapped, &u, sizeof(u));
  WriteSceneSet(slot);

  s_acc[2] += lap(t);
  RecordShadowPass(cmd, camera, city.day, realTime, slot);
  s_acc[3] += lap(t);
  RecordScenePass(cmd, camera, city.day, realTime, slot);
  s_acc[4] += lap(t);
  if (s_prof && ++s_frames >= 120)
  {
    std::printf("[prepare] build+upload %.2f  wave %.2f  uniforms %.2f  shadow %.2f  scene %.2f ms  (dyn verts %zu, lights %zu)\n",
      s_acc[0] / s_frames, s_acc[1] / s_frames, s_acc[2] / s_frames, s_acc[3] / s_frames, s_acc[4] / s_frames,
      m_dynamic.vertices.size(), m_frameLights.size());
    for (double& a : s_acc) a = 0.0;
    s_frames = 0;
  }
}

void CityRenderer::Record(const Frame& frame, City& city, const Camera& camera,
  const ToolPreview& /*tool*/, float realTime)
{
  NS_PROFILE_SCOPE("City::PostPass");
  if (m_preparedFrame != frame.frameIndex || !m_hdrValid)
    return; // nothing rendered this frame (no pre-scene hook): the clear colour shows
  if (m_postPipeline == VK_NULL_HANDLE || m_postSamples != frame.samples
    || m_postColorFormat != frame.colorFormat || m_postDepthFormat != frame.depthFormat)
    if (!CreatePostPipeline(frame.colorFormat, frame.depthFormat, frame.samples))
      return;
  const uint32_t slot = frame.frameIndex % kSlots;

  const float w = static_cast<float>(frame.extent.width), h = static_cast<float>(frame.extent.height);
  const float aspect = h > 0.0f ? w / h : 1.0f;
  const Mat4 proj = camera.Proj(aspect);
  const Mat4 viewProj = proj * camera.View();
  const Mat4 invViewProj = viewProj.Inverse();
  const DayLight L = ComputeDayLight(city.day);
  const Vec3 eye = camera.Eye(), fwd = camera.Forward();
  PostUniforms u = {};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
    {
      u.invViewProj[c][r] = invViewProj.m[c * 4 + r];
      u.viewProj[c][r] = viewProj.m[c * 4 + r];
    }
  u.cam[0] = eye.x; u.cam[1] = eye.y; u.cam[2] = eye.z; u.cam[3] = realTime;
  u.fwd[0] = fwd.x; u.fwd[1] = fwd.y; u.fwd[2] = fwd.z; u.fwd[3] = L.night;
  u.sun[0] = L.sunDir.x; u.sun[1] = L.sunDir.y; u.sun[2] = L.sunDir.z; u.sun[3] = 0.72f; // exposure
  u.sunColor[0] = L.sunColor.x; u.sunColor[1] = L.sunColor.y; u.sunColor[2] = L.sunColor.z;
  static const float s_debugView = static_cast<float>(std::atoi(std::getenv("CITYSLICE_DEBUGVIEW") ? std::getenv("CITYSLICE_DEBUGVIEW") : "0"));
  u.sunColor[3] = s_debugView; // 1 normals, 2 roughness, 3 depth, 4 no SSR
  u.fog[0] = L.horizon.x; u.fog[1] = L.horizon.y; u.fog[2] = L.horizon.z; u.fog[3] = proj.m[10];
  u.zenith[0] = L.zenith.x; u.zenith[1] = L.zenith.y; u.zenith[2] = L.zenith.z; u.zenith[3] = proj.m[14];
  u.size[0] = w; u.size[1] = h; u.size[2] = 1.0f / w; u.size[3] = 1.0f / h;
  std::memcpy(m_postUniforms[slot].mapped, &u, sizeof(u));

  VkCommandBuffer cmd = frame.cmd;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipelineLayout, 0, 1, &m_postSets[slot], 0, nullptr);
  VkViewport vp = {};
  vp.width = w;
  vp.height = h;
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor = {};
  scissor.extent = frame.extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}
