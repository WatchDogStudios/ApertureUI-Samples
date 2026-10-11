/*
 * CityRenderer.h - draws the CitySlice world (sky, terrain, water, roads,
 * bridges, buildings, service buildings, trees, vehicles, boats, tool preview,
 * overlays) through the Aperture Vulkan backend's two scene hooks.
 *
 * Frame:
 *   Prepare (pre-scene hook, before the swapchain pass)
 *     1. heightmap upload when the terrain changed
 *     2. water simulation step (compute, 3 rotating R32F images; impulses from
 *        the cursor, tools, boats)
 *     3. sun shadow map (depth-only, 2048)
 *     4. scene pass into an HDR target (RGBA16F colour + RGBA16F normal/roughness
 *        + D32 depth, 4x MSAA, all resolved) with PBR lighting, the shadow map,
 *        tile-gridded spotlights, foliage/water subsurface scattering
 *   Record (scene hook, inside the swapchain pass)
 *     5. post pass: screen-space reflections, environment specular, tonemap
 *
 * Geometry is flat-shaded vertex-colour triangles; the colour's alpha byte is
 * the material id (mirrored in city.slang), which picks roughness/metalness
 * and the procedural surface detail in the fragment shader.
 */
#pragma once

#include "CityAssets.h"
#include "CityMath.h"

#include <APPlayerVulkanContext.h>

#include <string>
#include <vector>

class City;
struct Camera;

class CityRenderer
{
public:
  using Frame = applayer::APPlayerVulkanContext::SceneFrame;

  /// What the active tool would touch, so the player sees it before committing.
  struct ToolPreview
  {
    int kind = 0; ///< see Tool in main.cpp (0 select, 1 road, 2-4 zones, 5 bulldoze, 6 avenue, 7-13 services, 14-17 terrain/trees)
    bool dragging = false;
    int x0 = 0, y0 = 0; ///< drag anchor tile
    int x1 = 0, y1 = 0; ///< current tile
    int hoverX = -1, hoverY = -1;
    int selX = -1, selY = -1; ///< tile picked with the select tool
    int brush = 2;            ///< terrain brush radius (tiles)
    int overlay = -1;         ///< service kind whose coverage to show, -1 none
  };

  // Material ids (vertex alpha byte), mirrored in city.slang.
  static constexpr uint32_t MatPlain = 255, MatUnlit = 250, MatWater = 240, MatGrass = 230,
                            MatFacade = 220, MatRoof = 210, MatRoad = 200, MatEmissive = 190,
                            MatFoliage = 180, MatBrick = 170, MatPlaster = 165, MatWindow = 160,
                            MatMetal = 150, MatSky = 140, MatConcrete = 130, MatPaint = 120,
                            MatOverlay = 110;

  bool Init(applayer::APPlayerVulkanContext& ctx, const std::string& spvDir);
  /// Loads the authored model library; without it everything stays procedural.
  void LoadAssets(const std::string& glbPath);
  void Shutdown();

  /// Terrain is cached; call after City::Reset().
  void InvalidateTerrain() { m_terrainBuilt = false; }

  /// CPU half of the frame: rebuilds meshes whose inputs changed, the dynamic
  /// layer, the overlay and the light grid. Touches no GPU state, so the game
  /// thread can run it while the HUD thread builds its command list. Optional:
  /// Prepare() runs it itself when it was not called this frame.
  void BuildCpu(City& city, const Camera& camera, const ToolPreview& tool, float realTime);
  /// Pre-scene hook: uploads what BuildCpu produced, steps the water, renders
  /// the shadow map and the HDR scene. Must run before Record() in the same frame.
  void Prepare(const Frame& frame, City& city, const Camera& camera, const ToolPreview& tool,
    float realTime);
  /// Swapchain-pass hook: the post pass (SSR + tonemap) over the HDR scene.
  void Record(const Frame& frame, City& city, const Camera& camera, const ToolPreview& tool,
    float realTime);

  /// Queue a ripple on the water for the next simulation step (world x/z, metres).
  void AddSplash(float x, float z, float radius, float strength);

  /// Horizon/fog colour for the current time of day (linear), also the clear colour.
  static Vec3 HorizonColor(double day);

private:
  static constexpr uint32_t kSlots = applayer::kMaxFramesInFlight;
  static constexpr uint32_t kAllSlots = (1u << kSlots) - 1u;
  static constexpr uint32_t kShadowRes = 2048;
  static constexpr uint32_t kWaveRes = 1024;
  static constexpr uint32_t kMaxLights = 1024;
  static constexpr uint32_t kMaxLightRefs = 24576;
  static constexpr int kMaxImpulses = 6;

  struct Vertex
  {
    float px, py, pz;
    float nx, ny, nz;
    uint32_t rgba; ///< R8G8B8A8_UNORM: bytes R,G,B in memory, A = material id
  };

  struct GpuBuffer
  {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    void* mapped = nullptr;
  };

  struct GpuImage
  {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
  };

  /// CPU mesh plus one GPU copy per frame-in-flight slot.
  struct Layer
  {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    GpuBuffer vb[kSlots];
    GpuBuffer ib[kSlots];
    uint32_t uploadedIndices[kSlots] = {};
    uint32_t dirtyMask = kAllSlots;
  };

  struct PushConstants // exactly 128 bytes, see city.slang
  {
    float col[4][4];
    float sun[4];      // xyz dir, w nightness
    float cam[4];      // xyz eye, w time
    float fog[4];      // xyz colour, w end distance
    float sunColor[4]; // xyz tint, w ambient scale
  };

  /// std140 block behind set 0 binding 0 (mirrored as `Scene` in city.slang).
  struct SceneUniforms
  {
    float light[4][4]; ///< sun view-projection, four columns
    float shadow[4];   ///< x strength 0..1, y texel size, z normal-offset metres, w unused
    float water[4];    ///< x level, y 1/map extent (world->uv), z wave amplitude, w wave texel
    float zenith[4];   ///< xyz sky zenith colour, w lights on 0..1
    float terrain[4];  ///< x 1/(N+1) (uv per corner), y 0.5/(N+1), z cell size, w tiles per side
  };

  /// std430 spotlight, mirrored in city.slang.
  struct Light
  {
    float px, py, pz, range;
    float dx, dy, dz, cosOuter;
    float r, g, b, cosInner;
  };

  /// std140 block for the post pass (mirrored as `Post` in post.slang).
  struct PostUniforms
  {
    float invViewProj[4][4];
    float viewProj[4][4];
    float cam[4];      ///< xyz eye, w time
    float fwd[4];      ///< xyz camera forward, w night
    float sun[4];      ///< xyz dir, w exposure
    float sunColor[4]; ///< xyz, w unused
    float fog[4];      ///< xyz horizon, w proj m10
    float zenith[4];   ///< xyz zenith, w proj m14
    float size[4];     ///< width, height, 1/width, 1/height
  };

  struct WavePush
  {
    float impulses[kMaxImpulses][4]; ///< uv x, uv y, radius (uv), strength
    float params[4];                 ///< damping, c^2, water level, impulse count
  };

  // ---- lifetime -------------------------------------------------------------
  bool CreateLayouts();
  bool CreateStaticResources(); ///< shadow map, wave images, heightmap, samplers, sets
  bool CreateHdrTargets(uint32_t width, uint32_t height);
  void DestroyHdrTargets();
  bool CreatePipeline(VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples,
    bool shadow, VkPipeline& out);
  bool CreatePostPipeline(VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
  bool CreateComputePipeline();
  VkShaderModule LoadModule(const std::string& path);
  bool EnsureBuffer(GpuBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage);
  void DestroyBuffer(GpuBuffer& buf);
  bool CreateImage(GpuImage& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
    VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
  void DestroyImage(GpuImage& img);
  void DestroyLayer(Layer& layer);
  void UploadLayer(Layer& layer, uint32_t slot);
  void DrawLayer(VkCommandBuffer cmd, Layer& layer, uint32_t slot);
  void WriteSceneSet(uint32_t slot);
  void WritePostSets();

  // ---- frame ---------------------------------------------------------------
  void UploadAll(uint32_t slot);
  void BuildLightGrid(double day);
  void UploadLights(uint32_t slot);
  void FillPushConstants(PushConstants& pc, const Mat4& viewProj, const Camera& camera,
    double day, float realTime) const;
  Mat4 ComputeLightViewProj(const Camera& camera, Vec3 sunDir) const;
  void RecordHeightmapUpload(VkCommandBuffer cmd, const City& city);
  void RecordWaveStep(VkCommandBuffer cmd, float dt);
  void RecordShadowPass(VkCommandBuffer cmd, const Camera& camera, double day, float realTime, uint32_t slot);
  void RecordScenePass(VkCommandBuffer cmd, const Camera& camera, double day, float realTime, uint32_t slot);
  void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
    VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess);

  // ---- meshes ----------------------------------------------------------------
  void BuildTerrain(const City& city);
  void BuildWater();
  void BuildCity(const City& city);      ///< roads, bridges, lots, street furniture: only when tiles change
  void BuildTrees(const City& city);     ///< wild + planted trees; rebuilt with the terrain
  void PruneTrees(const City& city);     ///< collapse trees on tiles that are no longer empty
  void BuildBuildings(const City& city, bool excludeRising); ///< static buildings
  void BuildDynamic(const City& city, const Camera& camera, const ToolPreview& tool, float realTime);
  void BuildOverlay(const City& city, const ToolPreview& tool);

  // Mesh helpers (CityModels.cpp). Flat shading: every face gets its own vertices.
  void AddFace(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, uint32_t color);
  void AddGroundQuad(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t color);
  void AddBox(Layer& l, Vec3 mn, Vec3 mx, uint32_t side, uint32_t top);
  /// Box with bevelled vertical and top edges: the building shell primitive.
  void AddChamferBox(Layer& l, Vec3 mn, Vec3 mx, float bevel, uint32_t side, uint32_t top);
  void AddTileFrame(Layer& l, const City& city, int x, int y, float lift, uint32_t color,
    float width = 0.7f);
  void AddBuilding(Layer& l, const City& city, int x, int y, float rise);
  void AddServiceBuilding(Layer& l, const City& city, int x, int y, float rise);
  /// Service model of an explicit kind (tool ghosts use tiles that hold nothing yet).
  void AddServiceModel(Layer& l, const City& city, int x, int y, int kind, int variant, float rise);
  void AddTree(Layer& l, float x, float y, float z, float scale, uint32_t seed);
  void AddCar(Layer& l, Vec3 centre, Vec3 dir, uint32_t color, bool night);
  void AddBoat(Layer& l, Vec3 centre, float heading, uint32_t color, float bob);
  void AddFaceN(Layer& l, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 na, Vec3 nb, Vec3 nc, Vec3 nd,
    uint32_t color);
  void AddTri(Layer& l, Vec3 a, Vec3 b, Vec3 c, uint32_t color);
  void AddCylinder(Layer& l, Vec3 baseCentre, float r0, float r1, float h, int sides,
    uint32_t color, bool cap);
  /// Cylinder along an arbitrary axis (wheels, pipes, masts).
  void AddCylinderAxis(Layer& l, Vec3 centre, Vec3 axis, float r, float len, int sides, uint32_t color);
  void AddSphere(Layer& l, Vec3 c, float r, int segs, int rings, uint32_t color, uint32_t seed,
    float jitter);
  /// Canopy blob: sphere whose normals point away from `nc` (the tree's centre) so
  /// several overlapping blobs shade as one soft mass.
  void AddCanopy(Layer& l, Vec3 c, Vec3 nc, float r, int segs, int rings, uint32_t color, uint32_t seed);
  void AddGable(Layer& l, Vec3 mn, Vec3 mx, float eave, float ridgeH, bool alongX, uint32_t wall,
    uint32_t roof, float overhang);
  void AddHip(Layer& l, Vec3 mn, Vec3 mx, float eave, float h, float inset, uint32_t roof,
    float overhang);
  void AddWindow(Layer& l, Vec3 centre, Vec3 normal, Vec3 right, float w, float h, uint32_t glass,
    uint32_t frame);
  void AddSkyDome(Layer& l, Vec3 eye, double day);
  void AddSpot(Vec3 pos, Vec3 dir, Vec3 color, float intensity, float range, float innerDeg, float outerDeg);
  /// Stamps an authored model into a layer. `yaw` turns the model's own front
  /// (+Z, the street side as authored in Blender) towards `forward`; `rise`
  /// scales it up from the ground for the spawn animation.
  void AddModel(Layer& l, const CityAssets::Model& model, Vec3 origin, Vec3 forward,
    float scale = 1.0f, float rise = 1.0f);
  /// Model for a zoned building, or nullptr when the library lacks it.
  const CityAssets::Model* BuildingModel(int type, int level) const;

  applayer::APPlayerVulkanContext* m_ctx = nullptr;
  VkDevice m_device = VK_NULL_HANDLE;
  std::string m_spvDir;

  // Scene pipelines.
  VkDescriptorSetLayout m_sceneSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_pipeline = VK_NULL_HANDLE;        ///< HDR scene (MSAA)
  VkPipeline m_shadowPipeline = VK_NULL_HANDLE;  ///< depth-only
  VkShaderModule m_vert = VK_NULL_HANDLE;
  VkShaderModule m_frag = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_sceneSamples = VK_SAMPLE_COUNT_4_BIT;

  // Post pass.
  VkDescriptorSetLayout m_postSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_postPipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_postPipeline = VK_NULL_HANDLE;
  VkShaderModule m_postVert = VK_NULL_HANDLE;
  VkShaderModule m_postFrag = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_postSamples = VK_SAMPLE_COUNT_1_BIT;
  VkFormat m_postColorFormat = VK_FORMAT_UNDEFINED;
  VkFormat m_postDepthFormat = VK_FORMAT_UNDEFINED;
  VkDescriptorSet m_postSets[kSlots] = {};
  GpuBuffer m_postUniforms[kSlots];

  // Water simulation.
  VkDescriptorSetLayout m_computeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
  VkPipeline m_wavePipeline = VK_NULL_HANDLE;
  VkShaderModule m_waveComp = VK_NULL_HANDLE;
  GpuImage m_wave[3];
  VkDescriptorSet m_waveSets[3] = {};
  int m_waveCur = 0; ///< index of the image holding the latest heights
  bool m_waveInitialised = false;
  std::vector<float> m_impulses; ///< groups of 4: uv x, uv y, radius uv, strength
  float m_waveAccum = 0.0f;

  // Descriptor pool + per-slot scene sets/uniforms.
  VkDescriptorPool m_descPool = VK_NULL_HANDLE;
  VkDescriptorSet m_sceneSets[kSlots] = {};
  GpuBuffer m_uniforms[kSlots];
  GpuBuffer m_lightBuf[kSlots];
  GpuBuffer m_gridBuf[kSlots];
  GpuBuffer m_indexBuf[kSlots];
  VkSampler m_linearSampler = VK_NULL_HANDLE;
  VkSampler m_shadowSampler = VK_NULL_HANDLE;

  // Shadow map.
  GpuImage m_shadow;
  Mat4 m_lightViewProj;
  bool m_shadowValid = false;

  // Terrain heightmap (R32F, (N+1)^2) + staging.
  GpuImage m_heightmap;
  GpuBuffer m_heightStaging;
  bool m_heightmapDirty = true;
  bool m_heightmapReady = false;

  // HDR targets (scene pass), recreated on resize.
  GpuImage m_hdrMsaa, m_hdr, m_auxMsaa, m_aux, m_depthMsaa, m_depth;
  uint32_t m_hdrWidth = 0, m_hdrHeight = 0;
  bool m_hdrValid = false;

  // Lights: static ones (lamps, signs) are gathered during mesh builds.
  std::vector<Light> m_staticLights;
  std::vector<Light> m_frameLights;
  std::vector<uint32_t> m_grid;
  std::vector<uint32_t> m_lightRefs;
  std::vector<Light>* m_spotSink = nullptr; ///< where AddSpot appends (lamps, buildings or per-frame)

  CityAssets m_assets;

  Layer m_terrain;
  Layer m_water;
  Layer m_city;
  Layer m_buildings;
  Layer m_dynamic;
  Layer m_overlay;
  Layer m_trees; ///< tiles that get built on have their trees collapsed in place
  std::vector<std::pair<uint32_t, uint32_t>> m_treeRange; ///< per tile: [vertexBegin, vertexEnd) or (0,0)
  std::vector<Light> m_lampLights;     ///< from BuildCity
  std::vector<Light> m_buildingLights; ///< from BuildBuildings
  bool m_wasRising = false;
  bool m_terrainBuilt = false;
  bool m_waterBuilt = false;
  int m_overlayKind = -2;
  uint32_t m_overlayVersion = ~0u;
  uint32_t m_preparedFrame = ~0u;
  bool m_cpuBuilt = false; ///< BuildCpu ran since the last Prepare
  Vec3 m_camEye;
};
