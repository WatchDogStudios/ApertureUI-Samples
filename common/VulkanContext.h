// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
/*
 * VulkanContext.h - Vulkan render backend for the samples.
 *
 * Design:
 *  - Vulkan 1.3 dynamic rendering: no VkRenderPass/VkFramebuffer objects, so the
 *    engine can switch between the swapchain and layer render targets mid-frame.
 *  - Every UpdateBuffer() lands in a per-frame ring allocator and Set*Buffer()
 *    binds that slice. SimpleAPHIRenderContext reuses ONE vertex/index buffer and
 *    ONE constant buffer per slot for every batch of a frame (D3D11 Map/Discard
 *    semantics); writing in place made every earlier draw read the last batch.
 *  - Frames open lazily: layer command lists are submitted while the engine is
 *    still building the main list (before BeginFrame), so the first GPU work of a
 *    frame may be a layer target, not the swapchain.
 *  - Engine-created images stay in VK_IMAGE_LAYOUT_GENERAL with coarse barriers.
 *  - Shaders are the SDK's HLSL (UIPlayer.hlsl, UIFilterCompute.hlsl) compiled
 *    by slangc to Shaders/Vulkan/*.spv; the SDK ships them compiled.
 */

#pragma once

#include "IPlayerRenderContext.h"

#include <APHarrlow/RenderPath/Implementation/SimpleAPHIRenderContext.h>
#include <APHarrlow/RenderPath/Interfaces/HarrlowCommandList.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Math/Rect.h>
#include <Foundation/Strings/String.h>
#ifdef APERTURE_HAS_VULKAN
#  if NS_ENABLED(NS_PLATFORM_WINDOWS)
#    define VK_USE_PLATFORM_WIN32_KHR
#  endif

#  include <vulkan/vulkan.h>

#  include <array>
#  include <functional>
#  include <mutex>
#  include <string>
#  include <unordered_map>
#  include <vector>

namespace HarrlowVG
{
  class Atlas;
}

namespace apui::samples
{
  static constexpr uint32_t kMaxFramesInFlight = 2;

  class VulkanContext final
    : public aperture::harrlow::renderpath::SimpleAPHIRenderContext,
      public IPlayerRenderContext
  {
  public:
    using APHIBuffer = aperture::harrlow::renderpath::APHIBuffer;
    using APHITexture = aperture::harrlow::renderpath::APHITexture;
    using APHIRenderTarget = aperture::harrlow::renderpath::APHIRenderTarget;
    using HarrlowCommandList = aperture::harrlow::renderpath::HarrlowCommandList;

    VulkanContext();
    ~VulkanContext() override;

    // ---- IPlayerRenderContext ----------------------------------------------
    bool Initialize(HWND windowHandle, nsUInt32 width, nsUInt32 height) override;
    void Shutdown() override;
    void BeginFrame(const nsColor& clearColor) override;
    void RenderCommands(const HarrlowCommandList& list) override;
    void SubmitCommands() override;
    void EndFrame(bool enableVSync) override;
    void Resize(nsUInt32 width, nsUInt32 height) override;
    void OnAtlasUpdated(HarrlowVG::Atlas* atlas) override;
    aperture::harrlow::renderpath::APHIRenderContext* GetRenderPathContext() override
    {
      return this;
    }
    void RequestBackbufferCapture(const char* bmpPath) override
    {
      m_capturePath = bmpPath ? bmpPath : "";
    }
    void SetViewportScreenOffset(float x, float y) override;

    // ---- Host scene hook -----------------------------------------------------
    // Fires inside BeginFrame() right after rendering to the swapchain image
    // begins and before any UI geometry, so a title can draw its world under the
    // HUD in the same pass. Depth is cleared to 1.0; the UI pipeline ignores depth.
    struct SceneFrame
    {
      VkCommandBuffer cmd;
      VkFormat colorFormat; ///< swapchain format (pipelines use dynamic rendering)
      VkFormat depthFormat; ///< depth-stencil attachment format
      VkExtent2D extent;
      VkSampleCountFlagBits
        samples; ///< sample count of the swapchain pass (see SetMSAASampleCount)
      uint32_t frameIndex; ///< 0..kMaxFramesInFlight-1; that slot's fence is waited
    };
    using SceneCallback = std::function<void(const SceneFrame&)>;
    void SetSceneCallback(SceneCallback cb) { m_sceneCallback = std::move(cb); }
    /// Fires every frame before the swapchain pass begins, with the frame's
    /// command buffer recording and no rendering active: the host may record
    /// its own passes here (shadow maps, reflections) that the scene callback
    /// then samples. Same SceneFrame contract as SetSceneCallback.
    void SetPreSceneCallback(SceneCallback cb) { m_preSceneCallback = std::move(cb); }

    /// False when BeginFrame() could not acquire a swapchain image; the host must
    /// skip RenderCommands/SubmitCommands/EndFrame for that frame.
    bool IsFrameOpen() const { return m_mainTargetActive; }
    /// For a host that builds UI command lists on a second thread: holds the
    /// frame API for the calling thread until the returned lock is destroyed or
    /// released. Take it around the WHOLE build (UIView::buildCommandList), not
    /// only the calls the backend sees: the build keeps state of its own between
    /// those calls (layer render-target bookkeeping, the scratch pool, a bound
    /// layer target) that a frame boundary on the game thread must not split.
    /// BeginFrame..EndFrame hold the same lock, so a build in progress delays the
    /// next frame instead of interleaving with it. The lock is recursive and the
    /// per-call locks inside the backend re-enter it. Do not wait for BeginFrame,
    /// EndFrame or Resize while you own it. Single-threaded hosts do not need it.
    std::unique_lock<std::recursive_mutex> LockRecording()
    {
      return std::unique_lock<std::recursive_mutex>(m_apiMutex);
    }
    VkDevice GetDevice() const { return m_device; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    VkQueue GetGraphicsQueue() const { return m_queue; }
    VkSampleCountFlagBits GetSampleCount() const { return m_msaaSamples; }
    uint32_t FindMemoryTypeIndex(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
    /// MSAA for the swapchain pass (the UI and the host scene). Layer render
    /// targets stay single-sampled. Call before Initialize() or at any time; the
    /// swapchain resources are rebuilt on the next BeginFrame().
    void SetMSAASampleCount(nsGALMSAASampleCount::Enum count);

    // ---- SimpleAPHIRenderContext overrides (resource creation) --------------
    nsObjectPtr<APHIBuffer> CreateVertexBuffer(const void* data, size_t size,
      size_t stride, const nsVariantDictionary& usage) override;
    nsObjectPtr<APHIBuffer> CreateIndexBuffer(const void* data, size_t size,
      const nsString& indexFormat, const nsVariantDictionary& usage) override;
    nsObjectPtr<APHITexture> CreateTexture(uint32_t width, uint32_t height,
      const nsString& format, const void* data, uint32_t rowPitch,
      const nsVariantDictionary& usage) override;
    nsObjectPtr<APHIRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height,
      const nsString& colorFormat, const nsString& depthFormat,
      const nsString& stencilFormat) override;
    void UpdateBuffer(
      APHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
    void UpdateTexture(APHITexture* texture, const void* data, uint32_t width,
      uint32_t height, uint32_t rowPitch) override;

    // ---- state ---------------------------------------------------------------
    void SetViewport(const nsRectFloat& rect, float minDepth, float maxDepth) override;
    void SetScissorRect(const nsRectU32& rect) override;
    void SetBlendState(bool enableBlend, const nsString& srcBlend,
      const nsString& dstBlend, const nsString& blendOp) override;
    void SetConstantBuffer(
      nsUInt32 slot, APHIBuffer* buffer, size_t offset, size_t size) override;
    void SetVertexBuffer(
      nsUInt32 slot, APHIBuffer* buffer, size_t offset, size_t stride) override;
    void SetIndexBuffer(APHIBuffer* buffer, size_t offset) override;
    void SetTexture(nsUInt32 slot, APHITexture* texture,
      aperture::harrlow::renderpath::APHISampler* sampler) override;
    void Draw(
      nsUInt32 vertexCount, nsUInt32 startVertex, nsUInt32 instanceCount) override;
    void DrawIndexed(nsUInt32 indexCount, nsUInt32 startIndex, nsInt32 baseVertex,
      nsUInt32 instanceCount) override;
    void WaitForGPU() override;
    void SubmitHarrlowCommands(const HarrlowCommandList& commandList) override;
    void ResolveTexture(APHITexture* msaaSrc, APHITexture* nonMsaaDst) override;
    void CopyTexture(APHITexture* src, APHITexture* dst) override;

  protected:
    void BindTargetTexture(
      APHITexture* texture, const HarrlowVG::Vec2& size, uintptr_t viewTag) override;
    void ReleaseTargetTexture(APHITexture* texture) override;
    APHITexture* UploadCanvasSurface(
      uint64_t handle, const CanvasSurfaceView& surface) override;
    APHITexture* UploadVideoSurface(
      uint64_t handle, const VideoSurfaceView& surface) override;
    APHITexture* ResolveExternalTextureHandle(HarrlowVG::TextureHandle handle) override;

    // compute filters
    void BindFilterComputeShader(aperture::harrlow::renderpath::FilterType type) override;
    void DoDispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
    void BindComputeOutputUAV(APHITexture* outputTexture) override;
    void BindComputeInputSRV(APHITexture* inputTexture) override;
    void BindComputeInputSRV1(APHITexture* inputTexture) override;
    void UnbindComputeResources() override;
    void BindComputeConstantBuffer(uint32_t slot, APHIBuffer* buffer) override;

  public:
    // ---- resource wrappers (public: the pools hold them by base pointer) ----
    class VulkanBuffer;
    class VulkanTexture;
    class VulkanRenderTarget;

    /// A slice of the per-frame ring; what Set*Buffer() binds.
    struct RingSlice
    {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceSize offset = 0;
      void* mapped = nullptr;
      uint32_t chunkIndex = 0;
    };

  private:
    // ---- setup ---------------------------------------------------------------
    bool CreateInstance();
    bool CreateSurface(HWND hwnd);
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateDepthBuffer();
    void DestroyDepthBuffer();
    void DestroyInstance();
    bool CreateFrames();
    bool CreateDescriptorLayouts();
    bool CreateShaders();
    bool CreateComputePipelines();
    bool CreateSamplerAndDefaults();
    VkShaderModule LoadSpirv(const char* relativePath);
    VkPipeline GetOrCreatePipeline(
      uint32_t blendKey, VkFormat colorFormat, bool hasDepth);

    // ---- frame / ring ---------------------------------------------------------
    struct RingChunk
    {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkDeviceSize capacity = 0;
      VkDeviceSize head = 0;
      void* mapped = nullptr;
      VkDescriptorSet uboSet = VK_NULL_HANDLE; ///< set 0 bound to this chunk
    };
    struct Frame
    {
      VkCommandBuffer cmd = VK_NULL_HANDLE;
      VkFence fence = VK_NULL_HANDLE;
      VkSemaphore imageAvailable = VK_NULL_HANDLE;
      VkSemaphore renderFinished = VK_NULL_HANDLE;
      VkDescriptorPool pool = VK_NULL_HANDLE;
      std::vector<RingChunk> ring;
      size_t ringChunk = 0;
      std::unordered_map<uint64_t, VkDescriptorSet> textureSets;
      std::vector<std::function<void()>>
        garbage; ///< destroyed once this frame's fence is waited
      bool open = false;
      bool acquired = false;
      bool submittedFence = false;
    };
    Frame& CurrentFrame() { return m_frames[m_frameIndex]; }
    void EnsureFrameOpen();
    RingSlice RingAlloc(VkDeviceSize size, VkDeviceSize align);
    bool AddRingChunk(Frame& frame, VkDeviceSize minCapacity);
    VkDescriptorSet GetUboSet(RingChunk& chunk);
    void DeferDestroy(std::function<void()> fn);
    void CollectGarbage(Frame& frame);

    // ---- rendering state ---------------------------------------------------------
    void BeginRenderingTo(VulkanTexture* target, bool clear);
    void BeginMainRendering(bool clear);
    void EndRendering();
    void EnsureRenderingActive();
    void FullBarrier(VkCommandBuffer cmd);
    void FlushDrawState();
    void ApplyViewportAndScissor();
    void WriteViewportConstants();
    VkDescriptorSet GetTextureSet(
      VulkanTexture* t0, VulkanTexture* t1, VulkanTexture* t2);
    /// Rows [rowBegin, rowEnd) of `pixels` (the whole image; rowEnd 0 = all
    /// rows) into the same rows of `tex`.
    void UploadImage(VulkanTexture* tex, const void* pixels, uint32_t rowPitchBytes,
      uint32_t rowBegin = 0, uint32_t rowEnd = 0);
    void StageBuffer(VulkanBuffer* buf);
    VulkanTexture* CreateTextureRaw(uint32_t width, uint32_t height, VkFormat format,
      const void* data, uint32_t rowPitch);
    VulkanTexture* PrepareAtlasTexture(HarrlowVG::Atlas* atlas);
    void EvictStaleSurfaces();
    void CaptureBackbuffer(VkImage image, VkFormat format);

    // ---- members -----------------------------------------------------------------
    HWND m_window = nullptr;
    uint32_t m_width = 0, m_height = 0;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_deviceProps = { };
    uint32_t m_queueFamily = 0;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D m_swapchainExtent = { };
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;
    std::vector<bool>
      m_swapchainPresented; ///< image has a defined layout from a previous present
    uint32_t m_imageIndex = 0;
    bool m_swapchainDirty = false;
    bool m_vsync = true;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    bool m_depthInitialised = false;
    VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImage m_msaaColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_msaaColorMemory = VK_NULL_HANDLE;
    VkImageView m_msaaColorView = VK_NULL_HANDLE;

    std::array<Frame, kMaxFramesInFlight> m_frames;
    uint32_t m_frameIndex = 0;
    uint64_t m_frameCounter = 0;

    VkDescriptorSetLayout m_uboLayout = VK_NULL_HANDLE; ///< set 0: 7 dynamic UBOs
    VkDescriptorSetLayout m_textureLayout
      = VK_NULL_HANDLE; ///< set 1: t0..t2, sampler, glyph buffers
    VkDescriptorSetLayout m_computeLayout
      = VK_NULL_HANDLE; ///< filters: ubo, in0, in1, out, sampler
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_vertModule = VK_NULL_HANDLE;
    VkShaderModule m_fragModule = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> m_pipelines;
    std::unordered_map<int, VkPipeline> m_computePipelines; ///< keyed by FilterType
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkBuffer m_dummyStorage = VK_NULL_HANDLE;
    VkDeviceMemory m_dummyStorageMemory = VK_NULL_HANDLE;

    /// GPU vector glyph storage buffers, bound at set 1 bindings 5..8:
    /// curve segments, COLRv1 paint commands, per-glyph band sets, band indices.
    /// Host-visible, re-created when the font engine marks them dirty or grows.
    struct GlyphBuffer
    {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkDeviceSize capacity = 0;
      void* mapped = nullptr; ///< persistently mapped (host-visible, coherent)
    };
    GlyphBuffer m_glyphBuffers[4];
    void UploadGlyphBuffers();
    bool UploadGlyphBuffer(
      GlyphBuffer& sb, const void* data, VkDeviceSize bytes, bool dirty);
    nsObjectPtr<VulkanTexture> m_whiteTexture;

    // current recording state
    bool m_renderingActive = false;         ///< inside vkCmdBeginRendering/End
    bool m_mainTargetActive = false;        ///< the swapchain image is the current target
    VulkanTexture* m_layerTarget = nullptr; ///< non-null while a layer RT is the target
    VkExtent2D m_targetExtent = { };
    VkFormat m_targetFormat = VK_FORMAT_UNDEFINED;
    nsColor m_clearColor = nsColor(0, 0, 0, 1);
    nsRectFloat m_viewport;
    VkRect2D m_scissor = { };
    float m_screenOffsetX = 0.0f, m_screenOffsetY = 0.0f;
    bool m_viewportDirty = true;

    uint32_t m_blendKey = 0; ///< packed enable|src|dst|op
    VkPipeline m_boundPipeline = VK_NULL_HANDLE;
    VkDeviceSize m_uboStride = 512; ///< per-binding slot inside a draw's constant block
    /// Constant data bound per shader binding (0..6); copied into the ring at draw time.
    std::array<std::pair<const void*, size_t>, 7> m_uboSources = { };
    RingSlice m_vertexSlice, m_indexSlice;
    VkDeviceSize m_vertexOffset = 0, m_indexOffset = 0;
    uint64_t m_vertexFrame = 0, m_indexFrame = 0;
    std::array<VulkanTexture*, 3> m_boundTextures = { };
    HarrlowCommandList m_pendingCommandList;
    bool m_shuttingDown = false;
    float m_viewportConstants[8] = { };

    // compute state
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    VulkanTexture* m_computeIn0 = nullptr;
    VulkanTexture* m_computeIn1 = nullptr;
    VulkanTexture* m_computeOut = nullptr;
    RingSlice m_computeUbo;
    VkDeviceSize m_computeUboSize = 0;

    // engine surfaces
    struct AtlasEntry
    {
      nsObjectPtr<VulkanTexture> texture;
      bool dirty = true;
    };
    struct SurfaceEntry
    {
      nsObjectPtr<VulkanTexture> texture;
      uint64_t generation = 0;
      uint64_t lastUsedFrame = 0;
      std::vector<uint8_t> rgba; ///< video YUV -> RGBA conversion scratch
    };
    std::unordered_map<const HarrlowVG::Atlas*, AtlasEntry> m_atlases;
    std::unordered_map<uint64_t, SurfaceEntry> m_canvases;
    std::unordered_map<uint64_t, SurfaceEntry> m_videos;

    SceneCallback m_sceneCallback;
    SceneCallback m_preSceneCallback;
    /// Serialises the frame API (BeginFrame..EndFrame, resource creation and
    /// uploads) so a host may build UI command lists on a second thread: the
    /// engine creates scratch render targets and uploads images from that
    /// build, and those record into the current frame's command buffer.
    std::recursive_mutex m_apiMutex;
    /// Leaf lock, never held across a call out. Guards Frame::garbage and, because
    /// DeferDestroy picks its slot from them, Frame::open and m_frameIndex
    /// (written under it in EndFrame and EnsureFrameOpen).
    std::mutex m_garbageMutex;
    /// Capacity kept between CollectGarbage calls (guarded by m_apiMutex).
    std::vector<std::function<void()>> m_garbageScratch;
    /// Held from BeginFrame() to EndFrame() so nothing on another thread can
    /// touch the frame while it is open (see the comment in BeginFrame).
    std::unique_lock<std::recursive_mutex> m_frameLock;
    std::string m_capturePath;
  };
} // namespace apui::samples
#endif
