// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// D3D11Context.h - Direct3D 11 render backend for the samples.

#pragma once

#include "IPlayerRenderContext.h"

#include <APHarrlow/RenderPath/Implementation/SimpleAPHIRenderContext.h>
#include <APHarrlow/RenderPath/Interfaces/HarrlowCommandList.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Math/Rect.h>
#include <Foundation/Strings/String.h>
#include <SDKBuildInfo.h> // APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace HarrlowVG
{
  class Atlas;
}

namespace apui::samples
{
  class D3D11Context final
    : public aperture::harrlow::renderpath::SimpleAPHIRenderContext,
      public IPlayerRenderContext
  {
  public:
    D3D11Context();
    ~D3D11Context() override;

    bool Initialize(HWND windowHandle, nsUInt32 width, nsUInt32 height) override;
    void Shutdown() override;

    void BeginFrame(const nsColor& clearColor) override;
    void RenderCommands(
      const aperture::harrlow::renderpath::HarrlowCommandList& list) override;
    void EndFrame(bool enableVSync) override;
    void RequestBackbufferCapture(const char* bmpPath) override
    {
      m_pendingCapturePath = bmpPath ? bmpPath : "";
    }
    void Resize(nsUInt32 width, nsUInt32 height) override;
    void SubmitCommands() override;
    void OnAtlasUpdated(HarrlowVG::Atlas* atlas) override;

    aperture::harrlow::renderpath::APHIRenderContext* GetRenderPathContext() override
    {
      return this;
    }
    void SetViewportScreenOffset(float x, float y) override;
    void SetMSAASampleCount(nsGALMSAASampleCount::Enum count);

    /// Enable/disable CSS-spec gamma-correct (linear-light) compositing.
    /// When enabled, the UIPlayer pixel shader decodes sRGB inputs to
    /// linear, blends in linear light, and re-encodes to sRGB at output,
    /// matching the color-managed surfaces of web browsers.  Off by
    /// default (byte-identical to the sRGB-direct path).
    void SetLinearColorManagement(bool enabled)
    {
      if (m_linearColorManagement != enabled)
      {
        m_linearColorManagement = enabled;
        m_viewportDirty = true;
      }
    }
    bool GetLinearColorManagement() const { return m_linearColorManagement; }

    /// Enable HDR ACES filmic tone mapping at the shader output.  Maps
    /// linear-light values above 1.0 (from additive blends, brightness
    /// boosts, emissive UI) into the displayable range with a filmic
    /// shoulder instead of hard-clipping.  Requires linear color management
    /// (the values must be in linear light); enabling HDR implies linear.
    /// This tone-maps for an SDR display; it does not create an HDR
    /// swapchain (FP16 backbuffer, ST.2084/scRGB output, luminance metadata).
    void SetHDRToneMapping(bool enabled)
    {
      if (m_hdrToneMapping != enabled)
      {
        m_hdrToneMapping = enabled;
        if (enabled)
          m_linearColorManagement = true; // HDR values must be linear
        m_viewportDirty = true;
      }
    }
    bool GetHDRToneMapping() const { return m_hdrToneMapping; }

    /// Create the device on this adapter instead of the system default. Call
    /// before Initialize(). OpenXR (XR_KHR_D3D11_enable) requires the device to
    /// live on the adapter the runtime reports for the headset.
    void SetAdapterLuid(const LUID& luid)
    {
      m_adapterLuid = luid;
      m_hasAdapterLuid = true;
    }

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetDeviceContext() const { return m_immediateContext.Get(); }

    void* GetContentSRV() override;
    void ResizeContent(nsUInt32 width, nsUInt32 height) override;
    void BindBackbuffer(const nsColor& clearColor) override;

  private:
    struct BlendKey
    {
      bool enable = false;
      D3D11_BLEND src = D3D11_BLEND_ONE;
      D3D11_BLEND dst = D3D11_BLEND_ZERO;
      D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD;

      bool operator==(const BlendKey& rhs) const
      {
        return enable == rhs.enable && src == rhs.src && dst == rhs.dst && op == rhs.op;
      }
    };

    struct BlendKeyHasher
    {
      size_t operator()(const BlendKey& key) const noexcept;
    };

    struct DepthStencilKey
    {
      bool depthEnable = false;
      bool depthWrite = false;
      bool stencilEnable = false;
      uint8_t stencilReadMask = 0xFF;
      uint8_t stencilWriteMask = 0xFF;
      D3D11_COMPARISON_FUNC stencilFunc = D3D11_COMPARISON_ALWAYS;
      D3D11_STENCIL_OP stencilPassOp = D3D11_STENCIL_OP_KEEP;
      D3D11_STENCIL_OP stencilFailOp = D3D11_STENCIL_OP_KEEP;

      bool operator==(const DepthStencilKey& rhs) const
      {
        return depthEnable == rhs.depthEnable && depthWrite == rhs.depthWrite
          && stencilEnable == rhs.stencilEnable && stencilReadMask == rhs.stencilReadMask
          && stencilWriteMask == rhs.stencilWriteMask && stencilFunc == rhs.stencilFunc
          && stencilPassOp == rhs.stencilPassOp && stencilFailOp == rhs.stencilFailOp;
      }
    };

    struct DepthStencilKeyHasher
    {
      size_t operator()(const DepthStencilKey& key) const noexcept;
    };

    bool CreateDeviceAndSwapChain(HWND windowHandle, nsUInt32 width, nsUInt32 height);
    bool CreateBackBufferResources();
    void ReleaseBackBufferResources();
    bool CompileDefaultShaders();
    bool CreateRasterizerState();
    bool CreateSamplerState();
    bool CreateWhiteTexture();
    bool CreateAtlasStateBuffer();
    bool CreateDepthStencilResources();
    void ReleaseDepthStencilResources();
    void EnsurePipelineState();
    void UpdateViewportConstants();
    void UpdateViewport(nsUInt32 width, nsUInt32 height);
    aperture::harrlow::renderpath::APHITexture* PrepareAtlasTexture(
      HarrlowVG::Atlas* atlas);
    aperture::harrlow::renderpath::APHITexture* PrepareCanvasTexture(
      uint64_t handle, const CanvasSurfaceView& surface);

    D3D11_BLEND StringToBlendFactor(const nsString& value) const;
    D3D11_BLEND_OP StringToBlendOp(const nsString& value) const;

    // SimpleAPHIRenderContext overrides
    nsObjectPtr<aperture::harrlow::renderpath::APHIBuffer> CreateVertexBuffer(
      const void* data, size_t size, size_t stride,
      const nsVariantDictionary& usage) override;
    nsObjectPtr<aperture::harrlow::renderpath::APHIBuffer> CreateIndexBuffer(
      const void* data, size_t size, const nsString& indexFormat,
      const nsVariantDictionary& usage) override;
    // Scratch/layer textures + render targets for CSS group compositing
    // (opacity groups, blend isolation, filters, masks). Without them
    // AcquireLayerRenderTarget returns null and the whole RT layer path is
    // skipped; composite-quad resolution and deferred RT release live in
    // the base class.
    nsObjectPtr<aperture::harrlow::renderpath::APHITexture> CreateTexture(uint32_t width,
      uint32_t height, const nsString& format, const void* data, uint32_t rowPitch,
      const nsVariantDictionary& usage) override;
    nsObjectPtr<aperture::harrlow::renderpath::APHIRenderTarget> CreateRenderTarget(
      uint32_t width, uint32_t height, const nsString& colorFormat,
      const nsString& depthFormat, const nsString& stencilFormat) override;
    void UpdateBuffer(aperture::harrlow::renderpath::APHIBuffer* buffer, const void* data,
      size_t size, size_t offset) override;
    void SetViewport(const nsRectFloat& rect, float minDepth, float maxDepth) override;
    void SetScissorRect(const nsRectU32& rect) override;
    void ResetScissorRect() override;
    void SetBlendState(bool enableBlend, const nsString& srcBlend,
      const nsString& dstBlend, const nsString& blendOp) override;
    void SetConstantBuffer(nsUInt32 slot,
      aperture::harrlow::renderpath::APHIBuffer* buffer, size_t offset,
      size_t size) override;
    void SetVertexBuffer(nsUInt32 slot, aperture::harrlow::renderpath::APHIBuffer* buffer,
      size_t offset, size_t stride) override;
    void SetIndexBuffer(
      aperture::harrlow::renderpath::APHIBuffer* buffer, size_t offset) override;
    void SetTexture(nsUInt32 slot, aperture::harrlow::renderpath::APHITexture* texture,
      aperture::harrlow::renderpath::APHISampler* sampler) override;
    void Draw(
      nsUInt32 vertexCount, nsUInt32 startVertex, nsUInt32 instanceCount) override;
    void DrawIndexed(nsUInt32 indexCount, nsUInt32 startIndex, nsInt32 baseVertex,
      nsUInt32 instanceCount) override;

    // Draw-record batching (see SimpleAPHIRenderContext).
    bool SupportsDrawRecords() const override { return m_drawRecordsSupported; }
    void SetDrawRecords(const void* records, size_t bytes, uint32_t stride) override;
    void BindTextureTable(
      const std::vector<aperture::harrlow::renderpath::APHITexture*>& textures) override;

    void WaitForGPU() override;
    void SubmitHarrlowCommands(
      const aperture::harrlow::renderpath::HarrlowCommandList& commandList) override;
    void UpdateAtlasStateConstant(bool isColorAtlas);
    bool CreateMSAAResources();
    void ReleaseMSAAResources();

    // Render-to-texture
    void SetRenderToTexture(bool enable) override;
    bool CreateRTTResources();
    void ReleaseRTTResources();

    // MSAA resolve for compute shader input
    void ResolveTexture(aperture::harrlow::renderpath::APHITexture* msaaSrc,
      aperture::harrlow::renderpath::APHITexture* nonMsaaDst) override;
    void CopyTexture(aperture::harrlow::renderpath::APHITexture* src,
      aperture::harrlow::renderpath::APHITexture* dst) override;

    // Compute shader dispatch for CSS filter effects
    void BindFilterComputeShader(aperture::harrlow::renderpath::FilterType type) override;
    void DoDispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
    void BindComputeOutputUAV(
      aperture::harrlow::renderpath::APHITexture* outputTexture) override;
    void BindComputeInputSRV(
      aperture::harrlow::renderpath::APHITexture* inputTexture) override;
    void BindComputeInputSRV1(
      aperture::harrlow::renderpath::APHITexture* inputTexture) override;
    void UnbindComputeResources() override;
    void BindComputeConstantBuffer(
      uint32_t slot, aperture::harrlow::renderpath::APHIBuffer* buffer) override;
    bool CaptureBackdropForBlend() override;

  protected:
    void BindTargetTexture(aperture::harrlow::renderpath::APHITexture* texture,
      const HarrlowVG::Vec2& size, uintptr_t viewTag) override;
    void ReleaseTargetTexture(
      aperture::harrlow::renderpath::APHITexture* texture) override;
    aperture::harrlow::renderpath::APHITexture* UploadCanvasSurface(
      uint64_t handle, const CanvasSurfaceView& surface) override;
    aperture::harrlow::renderpath::APHITexture* UploadVideoSurface(
      uint64_t handle, const VideoSurfaceView& surface) override;
    aperture::harrlow::renderpath::APHITexture* ResolveExternalTextureHandle(
      HarrlowVG::TextureHandle handle) override;

    // Clears the stencil buffer along with the base class's clip stack.
    void ClearStencilClips() override;

    // Depth-stencil state management
    ID3D11DepthStencilState* GetOrCreateDepthStencilState(const DepthStencilKey& key);
    void ApplyStencilState(bool stencilEnable, D3D11_COMPARISON_FUNC stencilFunc,
      D3D11_STENCIL_OP passOp, D3D11_STENCIL_OP failOp, uint8_t stencilRef);

    class D3D11Buffer final : public aperture::harrlow::renderpath::APHIBuffer
    {
    public:
      D3D11Buffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, size_t size,
        UINT bindFlags, D3D11_USAGE usage);

      size_t GetSize() const override { return m_size; }
      nsVariantDictionary GetUsage() const override { return { }; }
      void* GetNativeHandle() const override { return m_buffer.Get(); }
      void* Map(size_t offset = 0, size_t size = 0) override;
      void Unmap() override;

      ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }
      D3D11_USAGE GetD3DUsage() const { return m_usage; }
      UINT GetBindFlags() const { return m_bindFlags; }

    private:
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
      Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
      size_t m_size = 0;
      UINT m_bindFlags = 0;
      D3D11_USAGE m_usage = D3D11_USAGE_DEFAULT;
      void* m_mappedPtr = nullptr;
    };

    class D3D11Texture final : public aperture::harrlow::renderpath::APHITexture
    {
    public:
      D3D11Texture() = default;

      bool Initialize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
        bool allowRenderTarget = false);
      void Upload(ID3D11DeviceContext* context, const void* data, UINT rowPitch);
      // Upload only a sub-rect. `data` must point to the top-left pixel of the
      // region within the full source buffer; rowPitch is the FULL source row
      // pitch. Uploads w×h at (x,y).
      void UploadRegion(ID3D11DeviceContext* context, const void* data, UINT rowPitch,
        UINT x, UINT y, UINT w, UINT h);
      bool EnsureRenderTargetView(ID3D11Device* device);

      uint32_t GetWidth() const override { return m_width; }
      uint32_t GetHeight() const override { return m_height; }
      uint32_t GetDepth() const override { return 1u; }
      uint32_t GetMipLevels() const override { return m_mipLevels; }
      nsString GetFormat() const override { return m_formatString; }
      nsVariantDictionary GetUsage() const override { return { }; }
      void* GetNativeHandle() const override { return m_srv.Get(); }
      ID3D11RenderTargetView* GetRTV() const { return m_rtv.Get(); }
      ID3D11Texture2D* GetTexture() const { return m_texture.Get(); }
      bool SupportsRenderTarget() const { return m_allowRenderTarget; }

      ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
      ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
      DXGI_FORMAT GetFormatDXGI() const { return m_format; }
      void SetIsColorGlyphAtlas(bool value) { m_isColorGlyphAtlas = value; }
      bool IsColorGlyphAtlas() const { return m_isColorGlyphAtlas; }

    private:
      Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;
      UINT m_width = 0;
      UINT m_height = 0;
      UINT m_mipLevels = 1;
      DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
      nsString m_formatString;
      bool m_allowRenderTarget = false;
      bool m_isColorGlyphAtlas = false;
    };

    // Concrete APHIRenderTarget wrapping a render-target-capable D3D11Texture
    // as its color attachment. Handed out by CreateRenderTarget so the base
    // ScratchTexturePool can back off-screen layer compositing (opacity
    // groups, blend isolation, CSS filters, masks).
    class D3D11RenderTarget final : public aperture::harrlow::renderpath::APHIRenderTarget
    {
    public:
      D3D11RenderTarget(nsObjectPtr<D3D11Texture> color, uint32_t w, uint32_t h)
        : m_color(std::move(color))
        , m_width(w)
        , m_height(h)
      {
      }
      uint32_t GetWidth() const override { return m_width; }
      uint32_t GetHeight() const override { return m_height; }
      aperture::harrlow::renderpath::APHITexture* GetColorTexture() const override
      {
        return m_color.get();
      }
      aperture::harrlow::renderpath::APHITexture* GetDepthTexture() const override
      {
        return nullptr;
      }
      void* GetNativeHandle() const override
      {
        return m_color ? m_color->GetRTV() : nullptr;
      }

    private:
      nsObjectPtr<D3D11Texture> m_color;
      uint32_t m_width = 0;
      uint32_t m_height = 0;
    };

    struct AtlasEntry
    {
      nsObjectPtr<D3D11Texture> texture;
      bool dirty = true;
      // Accumulated dirty-rect UNION across all atlas updates since the last
      // upload.  Multiple glyph-add updates can land on one atlas between
      // draws; each reports its own sub-rect, so we union them here and
      // upload the combined box once at draw time.  hasDirtyRegion = false
      // means "upload the whole texture" (a full/resize update, or no
      // sub-rect info).  Half-open box: [L,R) x [T,B).
      bool hasDirtyRegion = false;
      int dirtyL = 0;
      int dirtyT = 0;
      int dirtyR = 0;
      int dirtyB = 0;
    };

    struct CanvasEntry
    {
      nsObjectPtr<D3D11Texture> texture;
      uint64_t uploadedGeneration = 0;
      // Frame this entry was last drawn with. EvictStaleSurfaceTextures()
      // drops entries that have not been drawn for a while, so every canvas,
      // decoded image, SVG, Lottie or video surface ever drawn does not keep
      // a live GPU texture for the life of the context.
      uint64_t lastUsedFrame = 0;
    };

    struct VideoEntry
    {
      uint64_t lastUsedFrame = 0; ///< see CanvasEntry::lastUsedFrame
      nsObjectPtr<D3D11Texture> texture;
      uint64_t uploadedGeneration = 0;
      std::vector<uint8_t> rgbaBuffer;
    };

    HWND m_window = nullptr;
    LUID m_adapterLuid = { };
    bool m_hasAdapterLuid = false;
    nsUInt32 m_width = 0;
    nsUInt32 m_height = 0;
    bool m_pipelineReady = false;
    bool m_viewportDirty = true;
    bool m_hasActiveBlendState = false;
    // CSS-spec gamma-correct compositing toggle (gColorMgmt.x in ViewportCB).
    bool m_linearColorManagement = false;
    // HDR ACES tone-map-at-output toggle (gColorMgmt.y in ViewportCB).
    bool m_hdrToneMapping = false;
    BlendKey m_activeBlendKey;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediateContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_blurHComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_blurVComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_dropShadowComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_dropShadowComposeComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_maskMultiplyComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_colorMatrixComputeShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    // Draw-record path: APUI_DRAW_RECORDS shader variant + per-draw records.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShaderRec;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShaderRec;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayoutRec;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_recordBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_recordSRV;
    size_t m_recordCapacity = 0;
    bool m_drawRecordsSupported = false;
    bool m_drawRecordsActive = false;
    bool m_recProgramBound = false;
    bool m_tableValid = false;
    ID3D11ShaderResourceView* m_boundTable[8] = { };
    void BindProgramForDraw();
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_viewportBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_filterBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_transformBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_gradientBuffer;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClampSampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_whiteTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteTextureSRV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_atlasStateBuffer;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_externalRenderTarget;

    // Depth-stencil resources (stencil state for clips; cleared every frame)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthStencilTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;

    std::unordered_map<BlendKey, Microsoft::WRL::ComPtr<ID3D11BlendState>, BlendKeyHasher>
      m_blendStates;
    std::unordered_map<const HarrlowVG::Atlas*, AtlasEntry> m_atlasTextures;
    std::unordered_map<uint64_t, CanvasEntry> m_canvasTextures;
    std::unordered_map<uint64_t, VideoEntry> m_videoTextures;

    /// Frame counter driving surface-texture eviction. Bumped in BeginFrame().
    uint64_t m_surfaceFrameIndex = 0;
    /// Drops surface textures not drawn for kSurfaceEvictionGraceFrames.
    void EvictStaleSurfaceTextures();

    // Depth-stencil state cache for stencil clip operations
    std::unordered_map<DepthStencilKey, Microsoft::WRL::ComPtr<ID3D11DepthStencilState>,
      DepthStencilKeyHasher>
      m_depthStencilStates;
    uint8_t m_currentStencilRef = 0;
    nsRectFloat m_currentViewport;
    aperture::harrlow::renderpath::HarrlowCommandList m_pendingCommandList;
    float m_cachedAtlasState = -1.0f;
    float m_viewportScreenOffsetX = 0.0f;
    float m_viewportScreenOffsetY = 0.0f;

    // MSAA resources
    nsGALMSAASampleCount::Enum m_msaaSampleCount = nsGALMSAASampleCount::None;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_msaaColorTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_msaaRTV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_msaaDepthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_msaaDSV;

    // Render-to-texture offscreen resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_rttTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rttRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_rttSRV;

    // Backdrop snapshot for in-shader non-separable CSS mix-blend-mode
    // compositing (overlay etc.). Lazily sized to match the RTT; the current RT
    // is CopyResource'd here and bound to PS slot 1 before a blend composite.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backdropTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_backdropSRV;

    // Set to true by BindBackbuffer() when the shell (ImGui) is active this frame.
    // EndFrame() skips the RTT->backbuffer CopyResource blit when this is true,
    // because ImGui already composites the RTT via ImGui::Image().
    bool m_backbufferBound = false;
    std::string m_pendingCapturePath; ///< one-shot BMP dump at next EndFrame

#if NS_ENABLED(APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING)
    // VectorGlyph curve StructuredBuffer (t5 in pixel shader)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vectorGlyphCurveBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_vectorGlyphCurveSRV;
    size_t m_vectorGlyphCurveBufferCapacity = 0;

    // VectorGlyph paint command StructuredBuffer (t6 in pixel shader, COLRv1)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vectorGlyphPaintCmdBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_vectorGlyphPaintCmdSRV;
    size_t m_vectorGlyphPaintCmdBufferCapacity = 0;
    // Vector glyph band acceleration: band directory (t7) + band curve-index list (t8).
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vectorGlyphBandBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_vectorGlyphBandSRV;
    size_t m_vectorGlyphBandBufferCapacity = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vectorGlyphBandIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_vectorGlyphBandIndexSRV;
    size_t m_vectorGlyphBandIndexBufferCapacity = 0;
#endif
  };
} // namespace apui::samples
