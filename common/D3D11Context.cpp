// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// D3D11Context.cpp - Direct3D 11 render backend for the samples.

#include "D3D11Context.h"
#pragma comment(lib, "dxguid.lib") // For D3D11 debugging interfaces
#include <APHTML/Interfaces/APCFileSystem.h>
#include <APHTML/Interfaces/APCPlatform.h>
#include <APHarrlow/VectorEngine/Core/Common.hpp>
#include <APHarrlow/VectorEngine/Core/Text.hpp>
#include <Foundation/Logging/Log.h>
#include <Foundation/Strings/StringUtils.h>
#include <d3dcompiler.h>

#if NS_ENABLED(APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING)
#  include <APHTML/Font/FontEngineBridge.h>
#  include <FontEngine/FontEngine.h>
#  include <FontEngine/VectorGlyphCurveBuffer.h>
#  include <FontEngine/VectorGlyphPaintEncoder.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aperture::harrlow::renderpath;
using Microsoft::WRL::ComPtr;

namespace
{
  // Shader file paths relative to uiresources
  static const char* kUIPlayerShaderPath = "Shaders/UIPlayer.hlsl";
  static const char* kUIFilterComputeShaderPath = "Shaders/UIFilterCompute.hlsl";

  // Cached shader sources (loaded once on first compile)
  static std::string s_cachedUIPlayerShader;
  static std::string s_cachedFilterComputeShader;

  bool LoadShaderFromFile(const char* relativePath, std::string& outSource)
  {
    auto* fileSystem = aperture::core::IAPCPlatform::GetEnginePlatform()->GetFileSystem();
    if (!fileSystem)
    {
      nsLog::Error("D3D11Context: FileSystem not available for shader loading");
      return false;
    }

    nsDataBuffer buffer = fileSystem->GetFileData(
      relativePath, aperture::core::IAPCFileSystem::EFileType::OSDependant);

    if (buffer.IsEmpty())
    {
      nsLog::Error("D3D11Context: Failed to load shader file: {0}", relativePath);
      return false;
    }

    outSource.assign(reinterpret_cast<const char*>(buffer.GetData()), buffer.GetCount());
    nsLog::Info(
      "D3D11Context: Loaded shader from {0} ({1} bytes)", relativePath, outSource.size());
    return true;
  }

  const char* GetUIPlayerShaderSource(size_t& outLength)
  {
    if (s_cachedUIPlayerShader.empty())
    {
      if (!LoadShaderFromFile(kUIPlayerShaderPath, s_cachedUIPlayerShader))
      {
        outLength = 0;
        return nullptr;
      }

      // Splice the SDK's GPU vector glyph coverage functions (the .hlsli
      // next to UIPlayer.hlsl) in at the marker. Missing file → zero-coverage
      // stub so the shader still compiles (vector glyphs render empty).
      const char* marker = "//__SLUG_CORE__";
      const size_t at = s_cachedUIPlayerShader.find(marker);
      if (at != std::string::npos)
      {
        std::string core;
        if (!LoadShaderFromFile("Shaders/UISlugCore.hlsli", core) || core.empty())
        {
          nsLog::Warning("D3D11Context: UISlugCore.hlsli missing; "
                         "GPU vector glyphs render as zero coverage");
          core
            = "float SlugAlphaCore(float2 p, float2 e, uint o, uint c) { return 0.0; }\n"
              "float SlugAlpha(float2 p, float o, float c) { return 0.0; }\n";
        }
        s_cachedUIPlayerShader.replace(at, strlen(marker), core);
      }
    }
    outLength = s_cachedUIPlayerShader.size();
    return s_cachedUIPlayerShader.c_str();
  }

  const char* GetFilterComputeShaderSource(size_t& outLength)
  {
    if (s_cachedFilterComputeShader.empty())
    {
      if (!LoadShaderFromFile(kUIFilterComputeShaderPath, s_cachedFilterComputeShader))
      {
        outLength = 0;
        return nullptr;
      }
    }
    outLength = s_cachedFilterComputeShader.size();
    return s_cachedFilterComputeShader.c_str();
  }

  HRESULT CompileShader(const char* source, size_t sourceLen, const char* entry,
    const char* target, ComPtr<ID3DBlob>& bytecode)
  {
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(source, sourceLen, nullptr, nullptr, nullptr, entry, target,
      flags, 0, &bytecode, &errors);
    if (FAILED(hr) && errors)
    {
      nsLog::Error("D3D11Context: Shader compile failed for {0}: {1}", entry,
        static_cast<const char*>(errors->GetBufferPointer()));
    }
    return hr;
  }

  bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
    const LUID* adapterLuid)
  {
    constexpr D3D_DRIVER_TYPE drivers[] = {
      D3D_DRIVER_TYPE_HARDWARE,
      D3D_DRIVER_TYPE_WARP,
      D3D_DRIVER_TYPE_REFERENCE,
    };
    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // A pinned adapter (an XR runtime names the GPU the headset is attached to)
    // must be honoured exactly: no fallback to another GPU or to WARP.
    if (adapterLuid)
    {
      ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
      {
        DXGI_ADAPTER_DESC1 desc = { };
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == adapterLuid->LowPart
          && desc.AdapterLuid.HighPart == adapterLuid->HighPart)
        {
          D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
          return SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                   nullptr, flags, featureLevels, _countof(featureLevels),
                   D3D11_SDK_VERSION, &device, &obtained, &context))
            && device && context;
        }
        adapter.Reset();
      }
      nsLog::Error("D3D11Context: no DXGI adapter matches the requested LUID");
      return false;
    }

    for (D3D_DRIVER_TYPE driver : drivers)
    {
      ComPtr<ID3D11Device> dev;
      ComPtr<ID3D11DeviceContext> ctx;
      D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
      HRESULT hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, featureLevels,
        _countof(featureLevels), D3D11_SDK_VERSION, &dev, &obtained, &ctx);
      if (SUCCEEDED(hr) && dev && ctx)
      {
        device = dev;
        context = ctx;
        return true;
      }
    }

    return false;
  }
} // namespace

namespace apui::samples
{
  D3D11Context::D3D11Context()
  {
    m_currentViewport.x = 0.0f;
    m_currentViewport.y = 0.0f;
    m_currentViewport.width = 1.0f;
    m_currentViewport.height = 1.0f;
  }

  D3D11Context::~D3D11Context()
  {
    Shutdown();
  }

  size_t D3D11Context::BlendKeyHasher::operator()(const BlendKey& key) const noexcept
  {
    size_t h = std::hash<int>()(key.enable ? 1 : 0);
    h ^= (std::hash<int>()(static_cast<int>(key.src)) << 1);
    h ^= (std::hash<int>()(static_cast<int>(key.dst)) << 2);
    h ^= (std::hash<int>()(static_cast<int>(key.op)) << 3);
    return h;
  }

  bool D3D11Context::Initialize(HWND windowHandle, nsUInt32 width, nsUInt32 height)
  {
    if (!windowHandle)
      return false;

    if (!CreateDeviceAndSwapChain(windowHandle, width, height))
      return false;

    if (!CompileDefaultShaders())
      return false;

    if (!CreateRasterizerState())
      return false;

    if (!CreateSamplerState())
      return false;

    if (!CreateWhiteTexture())
      return false;

    if (!CreateAtlasStateBuffer())
      return false;

    if (!CreateDepthStencilResources())
    {
      nsLog::Warning(
        "D3D11Context: Depth-stencil creation failed, stencil clips disabled");
      // Non-fatal - continue without stencil support
    }

    if (!SimpleAPHIRenderContext::Initialize(m_device.Get(), nsString("D3D11")))
      return false;

    UpdateViewport(width, height);
    SetRenderToTexture(true);
    return true;
  }

  bool D3D11Context::CreateDeviceAndSwapChain(
    HWND windowHandle, nsUInt32 width, nsUInt32 height)
  {
    m_window = windowHandle;
    m_width = width;
    m_height = height;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context, m_hasAdapterLuid ? &m_adapterLuid : nullptr))
      return false;

    m_device = device;
    m_immediateContext = context;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice)))
      return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
      return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
      return false;

    DXGI_SWAP_CHAIN_DESC1 desc = { };
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = { };
    fsDesc.Windowed = TRUE;

    if (FAILED(factory->CreateSwapChainForHwnd(
          m_device.Get(), windowHandle, &desc, &fsDesc, nullptr, &m_swapChain)))
      return false;

    factory->MakeWindowAssociation(windowHandle, DXGI_MWA_NO_ALT_ENTER);

    return CreateBackBufferResources();
  }

  bool D3D11Context::CreateBackBufferResources()
  {
    ReleaseBackBufferResources();

    if (!m_swapChain || !m_device)
      return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
      return false;

    if (FAILED(
          m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTarget)))
      return false;

    return true;
  }

  void D3D11Context::ReleaseBackBufferResources()
  {
    m_renderTarget.Reset();
  }

  bool D3D11Context::CompileDefaultShaders()
  {
    // Load shaders from file system
    size_t uiShaderLen = 0;
    const char* uiShaderSource = GetUIPlayerShaderSource(uiShaderLen);
    if (!uiShaderSource || uiShaderLen == 0)
    {
      nsLog::Error("D3D11Context: Failed to load UI shader from file");
      return false;
    }

    size_t filterShaderLen = 0;
    const char* filterShaderSource = GetFilterComputeShaderSource(filterShaderLen);
    if (!filterShaderSource || filterShaderLen == 0)
    {
      nsLog::Warning("D3D11Context: Failed to load filter compute shader from file");
      // Continue without filter shaders - they're optional
    }

    // Compile main UI shaders
    ComPtr<ID3DBlob> vsBytecode;
    if (FAILED(
          CompileShader(uiShaderSource, uiShaderLen, "VSMain", "vs_5_0", vsBytecode)))
      return false;

    ComPtr<ID3DBlob> psBytecode;
    if (FAILED(
          CompileShader(uiShaderSource, uiShaderLen, "PSMain", "ps_5_0", psBytecode)))
      return false;

    if (FAILED(m_device->CreateVertexShader(vsBytecode->GetBufferPointer(),
          vsBytecode->GetBufferSize(), nullptr, &m_vertexShader)))
      return false;

    if (FAILED(m_device->CreatePixelShader(psBytecode->GetBufferPointer(),
          psBytecode->GetBufferSize(), nullptr, &m_pixelShader)))
      return false;

    // Compile filter compute shaders (optional)
    if (filterShaderSource && filterShaderLen > 0)
    {
      ComPtr<ID3DBlob> csBlurHBytecode;
      if (FAILED(CompileShader(
            filterShaderSource, filterShaderLen, "CSBlurH", "cs_5_0", csBlurHBytecode)))
      {
        nsLog::Warning("D3D11Context: Failed to compile CSBlurH compute shader");
      }
      else if (FAILED(m_device->CreateComputeShader(csBlurHBytecode->GetBufferPointer(),
                 csBlurHBytecode->GetBufferSize(), nullptr, &m_blurHComputeShader)))
      {
        nsLog::Warning("D3D11Context: Failed to create CSBlurH compute shader");
      }

      ComPtr<ID3DBlob> csBlurVBytecode;
      if (FAILED(CompileShader(
            filterShaderSource, filterShaderLen, "CSBlurV", "cs_5_0", csBlurVBytecode)))
      {
        nsLog::Warning("D3D11Context: Failed to compile CSBlurV compute shader");
      }
      else if (FAILED(m_device->CreateComputeShader(csBlurVBytecode->GetBufferPointer(),
                 csBlurVBytecode->GetBufferSize(), nullptr, &m_blurVComputeShader)))
      {
        nsLog::Warning("D3D11Context: Failed to create CSBlurV compute shader");
      }

      ComPtr<ID3DBlob> csDropShadowBytecode;
      if (FAILED(CompileShader(filterShaderSource, filterShaderLen, "CSDropShadow",
            "cs_5_0", csDropShadowBytecode)))
      {
        nsLog::Warning("D3D11Context: Failed to compile CSDropShadow compute shader");
      }
      else if (FAILED(
                 m_device->CreateComputeShader(csDropShadowBytecode->GetBufferPointer(),
                   csDropShadowBytecode->GetBufferSize(), nullptr,
                   &m_dropShadowComputeShader)))
      {
        nsLog::Warning("D3D11Context: Failed to create CSDropShadow compute shader");
      }

      // Separable drop-shadow final compose pass (2-input: original + blurred).
      ComPtr<ID3DBlob> csDropShadowComposeBytecode;
      if (FAILED(CompileShader(filterShaderSource, filterShaderLen, "CSDropShadowCompose",
            "cs_5_0", csDropShadowComposeBytecode)))
      {
        nsLog::Warning(
          "D3D11Context: Failed to compile CSDropShadowCompose compute shader");
      }
      else if (FAILED(m_device->CreateComputeShader(
                 csDropShadowComposeBytecode->GetBufferPointer(),
                 csDropShadowComposeBytecode->GetBufferSize(), nullptr,
                 &m_dropShadowComposeComputeShader)))
      {
        nsLog::Warning(
          "D3D11Context: Failed to create CSDropShadowCompose compute shader");
      }

      // CSS mask-image alpha-multiply pass (2-input: layer + mask).
      ComPtr<ID3DBlob> csMaskMultiplyBytecode;
      if (FAILED(CompileShader(filterShaderSource, filterShaderLen, "CSMaskMultiply",
            "cs_5_0", csMaskMultiplyBytecode)))
      {
        nsLog::Warning("D3D11Context: Failed to compile CSMaskMultiply compute shader");
      }
      else if (FAILED(
                 m_device->CreateComputeShader(csMaskMultiplyBytecode->GetBufferPointer(),
                   csMaskMultiplyBytecode->GetBufferSize(), nullptr,
                   &m_maskMultiplyComputeShader)))
      {
        nsLog::Warning("D3D11Context: Failed to create CSMaskMultiply compute shader");
      }

      // CSS color-matrix filters (brightness/contrast/saturate/grayscale/
      // sepia/invert/hue-rotate) — a single kernel driven by the uploaded 4x4.
      ComPtr<ID3DBlob> csColorMatrixBytecode;
      if (FAILED(CompileShader(filterShaderSource, filterShaderLen, "CSColorMatrix",
            "cs_5_0", csColorMatrixBytecode)))
      {
        nsLog::Warning("D3D11Context: Failed to compile CSColorMatrix compute shader");
      }
      else if (FAILED(
                 m_device->CreateComputeShader(csColorMatrixBytecode->GetBufferPointer(),
                   csColorMatrixBytecode->GetBufferSize(), nullptr,
                   &m_colorMatrixComputeShader)))
      {
        nsLog::Warning("D3D11Context: Failed to create CSColorMatrix compute shader");
      }

      // Mark compute support as available if at least blur shaders compiled
      if (m_blurHComputeShader && m_blurVComputeShader)
        m_supportsCompute = true;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA,
        0 } };

    if (FAILED(m_device->CreateInputLayout(layout, _countof(layout),
          vsBytecode->GetBufferPointer(), vsBytecode->GetBufferSize(), &m_inputLayout)))
      return false;

    // Draw-record variant (SimpleAPHIRenderContext::SubmitWithDrawRecords):
    // one VS/PS for every shape kind, per-draw state from a structured buffer
    // at t9, textures from a table at t10..t17. Optional -- if it does not
    // build, SupportsDrawRecords() stays false and the per-batch path runs.
    // APUI_DRAW_RECORDS=0 in the environment also disables it at runtime.
    {
      const std::string recSource = std::string("#define APUI_DRAW_RECORDS 1\n")
        + std::string(uiShaderSource, uiShaderLen);
      ComPtr<ID3DBlob> vsRec, psRec;
      if (SUCCEEDED(
            CompileShader(recSource.c_str(), recSource.size(), "VSMain", "vs_5_0", vsRec))
        && SUCCEEDED(
          CompileShader(recSource.c_str(), recSource.size(), "PSMain", "ps_5_0", psRec)))
      {
        D3D11_INPUT_ELEMENT_DESC recLayout[] = { { "POSITION", 0,
                                                   DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                                   D3D11_INPUT_PER_VERTEX_DATA, 0 },
          { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA,
            0 },
          { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
            D3D11_INPUT_PER_VERTEX_DATA, 0 },
          { "DRAWID", 0, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        if (SUCCEEDED(m_device->CreateVertexShader(vsRec->GetBufferPointer(),
              vsRec->GetBufferSize(), nullptr, &m_vertexShaderRec))
          && SUCCEEDED(m_device->CreatePixelShader(psRec->GetBufferPointer(),
            psRec->GetBufferSize(), nullptr, &m_pixelShaderRec))
          && SUCCEEDED(m_device->CreateInputLayout(recLayout, _countof(recLayout),
            vsRec->GetBufferPointer(), vsRec->GetBufferSize(), &m_inputLayoutRec)))
          m_drawRecordsSupported = true;
      }
      if (!m_drawRecordsSupported)
        nsLog::Warning(
          "D3D11Context: draw-record shaders unavailable; per-batch path only");
    }

    D3D11_BUFFER_DESC cbDesc = { };
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.ByteWidth = sizeof(float) * 8;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_viewportBuffer)))
      return false;

    // Create filter constant buffer (for compute shaders)
    D3D11_BUFFER_DESC filterCbDesc = { };
    filterCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    filterCbDesc.ByteWidth = sizeof(float) * 16; // 4 float4s
    filterCbDesc.Usage = D3D11_USAGE_DYNAMIC;
    filterCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(m_device->CreateBuffer(&filterCbDesc, nullptr, &m_filterBuffer)))
      return false;

    // Create transform constant buffer (for vertex shader)
    D3D11_BUFFER_DESC transformCbDesc = { };
    transformCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    transformCbDesc.ByteWidth = sizeof(float) * 16; // 4 float4s (4x4 transform matrix)
    transformCbDesc.Usage = D3D11_USAGE_DYNAMIC;
    transformCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(m_device->CreateBuffer(&transformCbDesc, nullptr, &m_transformBuffer)))
      return false;

    // Create gradient constant buffer (slot b0) - supports 16-stop gradients
    // Layout: stops[16] * 4 floats + params[4] + dir[4] + center[4] + effects[4] = 80
    // floats = 320 bytes
    D3D11_BUFFER_DESC gradientCbDesc = { };
    gradientCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    gradientCbDesc.ByteWidth = sizeof(float) * 80; // 16 stops + params/dir/center/effects
    gradientCbDesc.Usage = D3D11_USAGE_DYNAMIC;
    gradientCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(m_device->CreateBuffer(&gradientCbDesc, nullptr, &m_gradientBuffer)))
      return false;

    // Debug labels for RenderDoc
    auto setDbgName = [](ID3D11DeviceChild* obj, const char* name)
    {
      if (obj)
        obj->SetPrivateData(
          WKPDID_D3DDebugObjectName, static_cast<UINT>(strlen(name)), name);
    };
    setDbgName(m_vertexShader.Get(), "UIPlayer_VS");
    setDbgName(m_pixelShader.Get(), "UIPlayer_PS");
    setDbgName(m_inputLayout.Get(), "UIPlayer_InputLayout");
    setDbgName(m_viewportBuffer.Get(), "CB1_ViewportCB");
    setDbgName(m_filterBuffer.Get(), "CB4_FilterCB");
    setDbgName(m_transformBuffer.Get(), "CB3_TransformCB");
    setDbgName(m_gradientBuffer.Get(), "CB0_GradientCB");

    return true;
  }

  void D3D11Context::Shutdown()
  {
    if (!m_device)
      return;

    m_immediateContext->ClearState();
    m_immediateContext->Flush();

    ReleaseBackBufferResources();
    ReleaseDepthStencilResources();
    ReleaseRTTResources();
    m_swapChain.Reset();
    m_viewportBuffer.Reset();
    m_filterBuffer.Reset();
    m_transformBuffer.Reset();
    m_gradientBuffer.Reset();
    m_inputLayout.Reset();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_blurHComputeShader.Reset();
    m_blurVComputeShader.Reset();
    m_dropShadowComputeShader.Reset();
    m_dropShadowComposeComputeShader.Reset();
    m_maskMultiplyComputeShader.Reset();
    m_colorMatrixComputeShader.Reset();
    m_rasterizerState.Reset();
    m_linearClampSampler.Reset();
    m_whiteTextureSRV.Reset();
    m_whiteTexture.Reset();
    m_atlasStateBuffer.Reset();
    m_atlasTextures.clear();
    m_canvasTextures.clear();
    m_videoTextures.clear();
    m_immediateContext.Reset();
    m_device.Reset();
    m_blendStates.clear();
    m_depthStencilStates.clear();
    m_currentStencilRef = 0;
    m_pipelineReady = false;
    m_viewportDirty = true;
    m_hasActiveBlendState = false;
    m_activeBlendKey = { };
    SimpleAPHIRenderContext::Shutdown();
  }

  namespace
  {
    // How many frames a surface texture may go undrawn before it is dropped.
    // Generous on purpose: the cost of being wrong is a re-upload next frame,
    // and it must comfortably exceed any in-flight frame depth so a texture is
    // never released while a submitted command list still references it.
    // ~2 seconds at 60fps.
    constexpr uint64_t kSurfaceEvictionGraceFrames = 120;
    // Only sweep occasionally — walking the maps every frame is pointless work.
    constexpr uint64_t kSurfaceEvictionIntervalFrames = 60;
  } // namespace

  void D3D11Context::EvictStaleSurfaceTextures()
  {
    if (m_surfaceFrameIndex < kSurfaceEvictionGraceFrames)
      return;
    const uint64_t cutoff = m_surfaceFrameIndex - kSurfaceEvictionGraceFrames;

    for (auto it = m_canvasTextures.begin(); it != m_canvasTextures.end();)
      it = (it->second.lastUsedFrame < cutoff) ? m_canvasTextures.erase(it) : ++it;

    for (auto it = m_videoTextures.begin(); it != m_videoTextures.end();)
      it = (it->second.lastUsedFrame < cutoff) ? m_videoTextures.erase(it) : ++it;
  }

  void D3D11Context::BeginFrame(const nsColor& clearColor)
  {
    // Bound the surface-texture caches. A game cycling through
    // inventory icons, per-screen art or video cutscenes otherwise accumulates
    // GPU-resident textures forever and eventually hits device-lost / OOM.
    // The CPU-side surface is still owned by its bridge, so an evicted texture
    // is simply recreated and re-uploaded the next time it is drawn.
    ++m_surfaceFrameIndex;
    if ((m_surfaceFrameIndex % kSurfaceEvictionIntervalFrames) == 0)
      EvictStaleSurfaceTextures();

    if (!m_immediateContext || !m_renderTarget)
      return;

    // ImGui rendering (in the shell) runs between EndFrame and the next
    // BeginFrame. It binds its own pixel-shader constant buffers and shader
    // resource views. Force a refresh of all cached GPU-side rendering state
    // so the harrlow pipeline does not skip uploads thinking it is still
    // current — this fixes intermittent missing glyphs on heavy text pages.
    m_cachedAtlasState = -1.0f;
    m_hasActiveBlendState = false;
    m_activeBlendKey = { };
    m_viewportDirty = true;

    // Defensively unbind all pixel-shader SRV slots that ImGui may have used,
    // so harrlow's draw calls start from a clean slot state.
    ID3D11ShaderResourceView* nullSRVs[8] = { nullptr };
    m_immediateContext->PSSetShaderResources(0, 8, nullSRVs);

    FLOAT clearArray[4] = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };

    if (m_renderToTexture && m_rttRTV)
    {
      // Render-to-texture mode: bind offscreen RT
      ID3D11RenderTargetView* rtvs[] = { m_rttRTV.Get() };
      m_immediateContext->OMSetRenderTargets(1, rtvs, m_depthStencilView.Get());
      m_immediateContext->ClearRenderTargetView(m_rttRTV.Get(), clearArray);
    }
    else if (m_msaaRTV)
    {
      ID3D11RenderTargetView* rtvs[] = { m_msaaRTV.Get() };
      m_immediateContext->OMSetRenderTargets(
        1, rtvs, m_msaaDSV ? m_msaaDSV.Get() : m_depthStencilView.Get());
      m_immediateContext->ClearRenderTargetView(m_msaaRTV.Get(), clearArray);
    }
    else
    {
      ID3D11RenderTargetView* rtv = m_renderTarget.Get();
      ID3D11DepthStencilView* dsv = m_depthStencilView.Get();
      m_immediateContext->OMSetRenderTargets(1, &rtv, dsv);
      m_immediateContext->ClearRenderTargetView(rtv, clearArray);
    }

    // Clear depth-stencil buffer for new frame
    ID3D11DepthStencilView* activeDSV
      = m_msaaDSV ? m_msaaDSV.Get() : m_depthStencilView.Get();
    if (activeDSV)
    {
      m_immediateContext->ClearDepthStencilView(
        activeDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
      m_currentStencilRef = 0;
    }

    nsRectFloat viewport(
      0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    SetViewport(viewport, 0.0f, 1.0f);
    SetScissorRect(nsRectU32(0, 0, m_width, m_height));
  }

  void D3D11Context::RenderCommands(const HarrlowCommandList& list)
  {
    // Main-list submission from the app: defer to SubmitCommands() so the
    // frame-end path can do atlas/curve-buffer prep first. Engine-internal
    // layer-RT lists never come through here — they call
    // SubmitHarrlowCommands directly and are executed immediately there.
    SubmitHarrlowCommands(list);
  }

  void D3D11Context::EndFrame(bool enableVSync)
  {
    if (m_swapChain)
    {
      if (m_backbufferBound)
      {
        // BindBackbuffer() was called this frame: the shell (ImGui) has already
        // rendered directly to the backbuffer and composited the RTT via
        // ImGui::Image().  Skip the CopyResource blit — it would overwrite ImGui.
        m_backbufferBound = false;
      }
      else if (m_renderToTexture && m_rttTexture)
      {
        // No shell active — copy RTT offscreen texture to backbuffer for display.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
        if (backBuffer)
          m_immediateContext->CopyResource(backBuffer.Get(), m_rttTexture.Get());
      }
      // Resolve MSAA to backbuffer
      else if (m_msaaColorTexture)
      {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
        if (backBuffer)
          m_immediateContext->ResolveSubresource(
            backBuffer.Get(), 0, m_msaaColorTexture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      }

      // One-shot live-frame capture (RequestBackbufferCapture): dump the composed
      // backbuffer BEFORE Present — after Present a flip-model backbuffer's
      // contents are undefined.
      if (!m_pendingCapturePath.empty())
      {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
        if (backBuffer && m_device && m_immediateContext)
        {
          D3D11_TEXTURE2D_DESC desc = { };
          backBuffer->GetDesc(&desc);
          desc.Usage = D3D11_USAGE_STAGING;
          desc.BindFlags = 0;
          desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          desc.MiscFlags = 0;
          desc.SampleDesc.Count = 1;
          desc.SampleDesc.Quality = 0;
          Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
          if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &staging)))
          {
            m_immediateContext->CopyResource(staging.Get(), backBuffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = { };
            if (SUCCEEDED(
                  m_immediateContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
              if (FILE* f = fopen(m_pendingCapturePath.c_str(), "wb"))
              {
                const int w = static_cast<int>(desc.Width);
                const int h = static_cast<int>(desc.Height);
                const int rowBytes = ((w * 3 + 3) / 4) * 4;
                const unsigned int dataSize = rowBytes * h;
                unsigned char hdr[54] = { };
                hdr[0] = 'B';
                hdr[1] = 'M';
                *reinterpret_cast<unsigned int*>(hdr + 2) = 54 + dataSize;
                *reinterpret_cast<unsigned int*>(hdr + 10) = 54;
                *reinterpret_cast<unsigned int*>(hdr + 14) = 40;
                *reinterpret_cast<int*>(hdr + 18) = w;
                *reinterpret_cast<int*>(hdr + 22) = h; // bottom-up
                *reinterpret_cast<unsigned short*>(hdr + 26) = 1;
                *reinterpret_cast<unsigned short*>(hdr + 28) = 24;
                *reinterpret_cast<unsigned int*>(hdr + 34) = dataSize;
                fwrite(hdr, 1, 54, f);
                std::vector<unsigned char> row(rowBytes, 0);
                for (int y = h - 1; y >= 0; --y)
                {
                  const unsigned char* src
                    = static_cast<const unsigned char*>(mapped.pData)
                    + static_cast<size_t>(y) * mapped.RowPitch;
                  for (int x = 0; x < w; ++x)
                  {
                    // backbuffer is RGBA8 → BMP wants BGR
                    row[x * 3 + 0] = src[x * 4 + 2];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 0];
                  }
                  fwrite(row.data(), 1, rowBytes, f);
                }
                fclose(f);
              }
              m_immediateContext->Unmap(staging.Get(), 0);
            }
          }
        }
        m_pendingCapturePath.clear();
      }

      m_swapChain->Present(enableVSync ? 1 : 0, 0);
    }
  }

  void D3D11Context::Resize(nsUInt32 width, nsUInt32 height)
  {
    if (!m_swapChain)
      return;

    m_width = width;
    m_height = height;
    ReleaseMSAAResources();
    ReleaseBackBufferResources();
    ReleaseDepthStencilResources();
    if (m_renderToTexture)
      ReleaseRTTResources();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateBackBufferResources();
    CreateDepthStencilResources();
    CreateMSAAResources();
    if (m_renderToTexture)
      CreateRTTResources();
    UpdateViewport(width, height);
  }

  void D3D11Context::UpdateViewport(nsUInt32 width, nsUInt32 height)
  {
    m_currentViewport.x = 0.0f;
    m_currentViewport.y = 0.0f;
    m_currentViewport.width = static_cast<float>(width);
    m_currentViewport.height = static_cast<float>(height);
    m_viewportDirty = true;
    m_pipelineReady = false;
    m_hasActiveBlendState = false;
    m_activeBlendKey = { };
  }

  void D3D11Context::EnsurePipelineState()
  {
    if (!m_immediateContext || m_pipelineReady)
      return;

    ID3D11VertexShader* vs = m_vertexShader.Get();
    ID3D11PixelShader* ps = m_pixelShader.Get();
    ID3D11InputLayout* layout = m_inputLayout.Get();

    if (!vs || !ps || !layout)
      return;

    m_immediateContext->IASetInputLayout(layout);
    m_immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_immediateContext->VSSetShader(vs, nullptr, 0);
    m_immediateContext->PSSetShader(ps, nullptr, 0);
    m_recProgramBound = false;

    ID3D11Buffer* vpBuffer = m_viewportBuffer.Get();
    if (vpBuffer)
    {
      m_immediateContext->VSSetConstantBuffers(1, 1, &vpBuffer);
      m_immediateContext->PSSetConstantBuffers(
        1, 1, &vpBuffer); // Also bind to PS for blur effects
    }

    ID3D11RasterizerState* rs = m_rasterizerState.Get();
    if (rs)
      m_immediateContext->RSSetState(rs);

    // NOTE: Per-batch constant buffers (slots 0, 2, 3, 4) are NOT bound here.
    // They are managed per-batch by SimpleAPHIRenderContext::SubmitHarrlowCommands
    // which uploads the correct data (gradient, atlas, transform, filter) before
    // every DrawIndexed call. Binding defaults here would overwrite per-batch
    // values since EnsurePipelineState runs lazily from DrawIndexed.

    m_pipelineReady = true;
  }

  bool D3D11Context::CreateRasterizerState()
  {
    if (!m_device)
      return false;

    D3D11_RASTERIZER_DESC desc = { };
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = TRUE;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;
    if (FAILED(m_device->CreateRasterizerState(&desc, &state)))
      return false;

    m_rasterizerState = state;
    m_pipelineReady = false;
    return true;
  }

  bool D3D11Context::CreateSamplerState()
  {
    if (!m_device)
      return false;

    D3D11_SAMPLER_DESC desc = { };
    desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(m_device->CreateSamplerState(&desc, &sampler)))
      return false;

    m_linearClampSampler = sampler;
    return true;
  }

  bool D3D11Context::CreateWhiteTexture()
  {
    if (!m_device)
      return false;

    D3D11_TEXTURE2D_DESC desc = { };
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // White pixel with full alpha (RGBA = 255, 255, 255, 255)
    uint8_t white[4] = { 255, 255, 255, 255 };
    D3D11_SUBRESOURCE_DATA init = { };
    init.pSysMem = white;
    init.SysMemPitch = sizeof(white);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m_device->CreateTexture2D(&desc, &init, &texture)))
      return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv)))
      return false;

    m_whiteTexture = texture;
    m_whiteTextureSRV = srv;
    return true;
  }

  bool D3D11Context::CreateAtlasStateBuffer()
  {
    if (!m_device)
      return false;

    D3D11_BUFFER_DESC desc = { };
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.ByteWidth
      = sizeof(float) * 8; // isColorAtlas, isVideoYUV, isSDFText, sdfScale, isSDFBorder,
                           // isSDFBoxShadow, isSDFBackground, isSDFShape
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, nullptr, &buffer)))
      return false;

    m_atlasStateBuffer = buffer;
    m_cachedAtlasState = -1.0f;
    UpdateAtlasStateConstant(false);
    return true;
  }

  bool D3D11Context::CreateDepthStencilResources()
  {
    if (!m_device || m_width == 0 || m_height == 0)
      return false;

    ReleaseDepthStencilResources();

    // Create D24S8 depth-stencil texture
    D3D11_TEXTURE2D_DESC texDesc = { };
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_depthStencilTexture)))
    {
      nsLog::Error("D3D11Context: Failed to create depth-stencil texture");
      return false;
    }

    // Create depth-stencil view
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = { };
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = 0;
    dsvDesc.Texture2D.MipSlice = 0;

    if (FAILED(m_device->CreateDepthStencilView(
          m_depthStencilTexture.Get(), &dsvDesc, &m_depthStencilView)))
    {
      nsLog::Error("D3D11Context: Failed to create depth-stencil view");
      m_depthStencilTexture.Reset();
      return false;
    }

    nsLog::Debug(
      "D3D11Context: Created D24S8 depth-stencil ({0}x{1})", m_width, m_height);
    return true;
  }

  void D3D11Context::ReleaseDepthStencilResources()
  {
    m_depthStencilView.Reset();
    m_depthStencilTexture.Reset();
  }

  size_t D3D11Context::DepthStencilKeyHasher::operator()(
    const DepthStencilKey& key) const noexcept
  {
    size_t h = std::hash<int>()(key.depthEnable ? 1 : 0);
    h ^= (std::hash<int>()(key.depthWrite ? 1 : 0) << 1);
    h ^= (std::hash<int>()(key.stencilEnable ? 1 : 0) << 2);
    h ^= (std::hash<int>()(key.stencilReadMask) << 3);
    h ^= (std::hash<int>()(key.stencilWriteMask) << 4);
    h ^= (std::hash<int>()(static_cast<int>(key.stencilFunc)) << 5);
    h ^= (std::hash<int>()(static_cast<int>(key.stencilPassOp)) << 6);
    h ^= (std::hash<int>()(static_cast<int>(key.stencilFailOp)) << 7);
    return h;
  }

  ID3D11DepthStencilState* D3D11Context::GetOrCreateDepthStencilState(
    const DepthStencilKey& key)
  {
    auto it = m_depthStencilStates.find(key);
    if (it != m_depthStencilStates.end())
      return it->second.Get();

    D3D11_DEPTH_STENCIL_DESC desc = { };
    desc.DepthEnable = key.depthEnable ? TRUE : FALSE;
    desc.DepthWriteMask
      = key.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D11_COMPARISON_LESS;
    desc.StencilEnable = key.stencilEnable ? TRUE : FALSE;
    desc.StencilReadMask = key.stencilReadMask;
    desc.StencilWriteMask = key.stencilWriteMask;

    // Front face stencil operations
    desc.FrontFace.StencilFunc = key.stencilFunc;
    desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    desc.FrontFace.StencilPassOp = key.stencilPassOp;
    desc.FrontFace.StencilFailOp = key.stencilFailOp;

    // Back face - same as front for 2D UI
    desc.BackFace = desc.FrontFace;

    ComPtr<ID3D11DepthStencilState> state;
    if (FAILED(m_device->CreateDepthStencilState(&desc, &state)))
    {
      nsLog::Error("D3D11Context: Failed to create depth-stencil state");
      return nullptr;
    }

    m_depthStencilStates[key] = state;
    return state.Get();
  }

  void D3D11Context::ApplyStencilState(bool stencilEnable,
    D3D11_COMPARISON_FUNC stencilFunc, D3D11_STENCIL_OP passOp, D3D11_STENCIL_OP failOp,
    uint8_t stencilRef)
  {
    if (!m_immediateContext)
      return;

    DepthStencilKey key;
    key.depthEnable = false;
    key.depthWrite = false;
    key.stencilEnable = stencilEnable;
    key.stencilReadMask = 0xFF;
    key.stencilWriteMask = 0xFF;
    key.stencilFunc = stencilFunc;
    key.stencilPassOp = passOp;
    key.stencilFailOp = failOp;

    ID3D11DepthStencilState* state = GetOrCreateDepthStencilState(key);
    if (state)
    {
      m_immediateContext->OMSetDepthStencilState(state, stencilRef);
      m_currentStencilRef = stencilRef;
    }
  }

  void D3D11Context::ClearStencilClips()
  {
    if (!m_immediateContext || !m_depthStencilView)
    {
      SimpleAPHIRenderContext::ClearStencilClips();
      return;
    }

    // Clear stencil buffer
    m_immediateContext->ClearDepthStencilView(
      m_depthStencilView.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Call base to clear stack
    SimpleAPHIRenderContext::ClearStencilClips();

    // Disable stencil testing
    ApplyStencilState(
      false, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, 0);
  }

  void D3D11Context::UpdateViewportConstants()
  {
    if (!m_viewportDirty || !m_immediateContext || !m_viewportBuffer)
      return;

    D3D11_MAPPED_SUBRESOURCE mapped = { };
    HRESULT hr = m_immediateContext->Map(
      m_viewportBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
      return;

    float* constants = static_cast<float*>(mapped.pData);
    constants[0] = m_currentViewport.x;
    constants[1] = m_currentViewport.y;
    constants[2] = m_currentViewport.width <= 0.0f ? 1.0f : m_currentViewport.width;
    constants[3] = m_currentViewport.height <= 0.0f ? 1.0f : m_currentViewport.height;
    // gColorMgmt.x: 1.0 = linear-correct (gamma) compositing, 0.0 = sRGB direct.
    // gColorMgmt.y: 1.0 = HDR ACES tone-map at output (requires linear mode).
    constants[4] = m_linearColorManagement ? 1.0f : 0.0f;
    constants[5] = m_hdrToneMapping ? 1.0f : 0.0f;
    constants[6] = 0.0f;
    constants[7] = 0.0f;

    m_immediateContext->Unmap(m_viewportBuffer.Get(), 0);
    m_viewportDirty = false;
  }

  D3D11Context::D3D11Buffer::D3D11Buffer(ID3D11DeviceContext* context,
    ID3D11Buffer* buffer, size_t size, UINT bindFlags, D3D11_USAGE usage)
    : m_context(context)
    , m_buffer(buffer)
    , m_size(size)
    , m_bindFlags(bindFlags)
    , m_usage(usage)
  {
  }

  void* D3D11Context::D3D11Buffer::Map(size_t, size_t)
  {
    if (!m_context || !m_buffer)
      return nullptr;

    D3D11_MAP mapType
      = (m_usage == D3D11_USAGE_DYNAMIC) ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE;
    D3D11_MAPPED_SUBRESOURCE mapped = { };
    HRESULT hr = m_context->Map(m_buffer.Get(), 0, mapType, 0, &mapped);
    if (FAILED(hr))
      return nullptr;

    m_mappedPtr = mapped.pData;
    return m_mappedPtr;
  }

  void D3D11Context::D3D11Buffer::Unmap()
  {
    if (!m_context || !m_buffer || !m_mappedPtr)
      return;

    m_context->Unmap(m_buffer.Get(), 0);
    m_mappedPtr = nullptr;
  }

  bool D3D11Context::D3D11Texture::Initialize(ID3D11Device* device, UINT width,
    UINT height, DXGI_FORMAT format, bool allowRenderTarget)
  {
    if (!device || width == 0 || height == 0)
      return false;

    m_texture.Reset();
    m_srv.Reset();
    m_rtv.Reset();

    D3D11_TEXTURE2D_DESC desc = { };
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (allowRenderTarget)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_texture)))
      return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_srv)))
      return false;

    if (allowRenderTarget)
    {
      if (FAILED(device->CreateRenderTargetView(m_texture.Get(), nullptr, &m_rtv)))
        return false;

      // Create UAV for compute shader write access (CSS filter effects)
      D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = { };
      uavDesc.Format = format;
      uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      uavDesc.Texture2D.MipSlice = 0;
      if (FAILED(device->CreateUnorderedAccessView(m_texture.Get(), &uavDesc, &m_uav)))
      {
        // Non-fatal; compute filters just won't work on this texture
        m_uav.Reset();
      }
    }

    m_width = width;
    m_height = height;
    m_mipLevels = 1;
    m_format = format;
    m_allowRenderTarget = allowRenderTarget;

    switch (format)
    {
      case DXGI_FORMAT_R8_UNORM:
        m_formatString = "R8_UNORM";
        break;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        m_formatString = "R8G8B8A8_UNORM";
        break;
      default:
        m_formatString = "UNKNOWN";
        break;
    }

    return true;
  }

  void D3D11Context::D3D11Texture::Upload(
    ID3D11DeviceContext* context, const void* data, UINT rowPitch)
  {
    if (!context || !m_texture || !data)
      return;

    context->UpdateSubresource(m_texture.Get(), 0, nullptr, data, rowPitch, 0);
  }

  void D3D11Context::D3D11Texture::UploadRegion(ID3D11DeviceContext* context,
    const void* data, UINT rowPitch, UINT x, UINT y, UINT w, UINT h)
  {
    if (!context || !m_texture || !data || w == 0 || h == 0)
      return;

    // Clamp the destination box to the texture extent (defensive — a stale
    // dirty rect must never write out of bounds).
    if (x >= m_width || y >= m_height)
      return;
    if (x + w > m_width)
      w = m_width - x;
    if (y + h > m_height)
      h = m_height - y;

    D3D11_BOX box;
    box.left = x;
    box.top = y;
    box.front = 0;
    box.right = x + w;
    box.bottom = y + h;
    box.back = 1;
    // `data` already points at the (x,y) pixel; SrcRowPitch is the full source
    // row pitch so D3D walks rows correctly within the larger source buffer.
    context->UpdateSubresource(m_texture.Get(), 0, &box, data, rowPitch, 0);
  }

  bool D3D11Context::D3D11Texture::EnsureRenderTargetView(ID3D11Device* device)
  {
    if (!m_allowRenderTarget || !device || !m_texture)
      return false;
    if (m_rtv)
      return true;
    return SUCCEEDED(device->CreateRenderTargetView(m_texture.Get(), nullptr, &m_rtv));
  }

  namespace
  {
    // Map the ScratchTexturePool / paint-layer format strings to DXGI. Only
    // the color formats the RT/filter path actually requests are handled;
    // anything else falls back to RGBA8 (the layer/scratch default).
    DXGI_FORMAT LayerFormatToDXGI(const nsString& format)
    {
      if (nsStringUtils::IsEqual_NoCase(format.GetData(), "R8")
        || nsStringUtils::IsEqual_NoCase(format.GetData(), "R8_UNORM"))
        return DXGI_FORMAT_R8_UNORM;
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
  } // namespace

  nsObjectPtr<APHITexture> D3D11Context::CreateTexture(uint32_t width, uint32_t height,
    const nsString& format, const void* data, uint32_t rowPitch,
    const nsVariantDictionary&)
  {
    if (!m_device || width == 0 || height == 0)
      return { };

    const DXGI_FORMAT dxgi = LayerFormatToDXGI(format);

    // Scratch + off-screen layer textures arrive with no initial data and an
    // RGBA8 format (the only callers are the ScratchTexturePool and layer
    // compositing); give them RTV+UAV so they can serve as render targets
    // and compute-filter surfaces. Uploaded textures stay plain SRVs.
    const bool allowRenderTarget
      = (data == nullptr) && (dxgi == DXGI_FORMAT_R8G8B8A8_UNORM);

    auto* tex = new D3D11Texture();
    if (!tex->Initialize(m_device.Get(), width, height, dxgi, allowRenderTarget))
    {
      delete tex;
      return { };
    }
    if (data)
      tex->Upload(m_immediateContext.Get(), data, rowPitch ? rowPitch : width * 4);

    return nsObjectPtr<APHITexture>(tex);
  }

  nsObjectPtr<APHIRenderTarget> D3D11Context::CreateRenderTarget(uint32_t width,
    uint32_t height, const nsString& colorFormat, const nsString& /*depthFormat*/,
    const nsString& /*stencilFormat*/)
  {
    if (!m_device || width == 0 || height == 0)
      return { };

    nsObjectPtr<D3D11Texture> color;
    color.reset(new D3D11Texture());
    if (!color->Initialize(m_device.Get(), width, height, LayerFormatToDXGI(colorFormat),
          /*allowRenderTarget*/ true))
      return { };
    if (!color->GetRTV())
      return { };

    auto* rt = new D3D11RenderTarget(std::move(color), width, height);
    return nsObjectPtr<APHIRenderTarget>(rt);
  }

  nsObjectPtr<APHIBuffer> D3D11Context::CreateVertexBuffer(
    const void* data, size_t size, size_t stride, const nsVariantDictionary&)
  {
    if (!m_device || size == 0)
      return { };

    D3D11_BUFFER_DESC desc = { };
    bool isConstant = stride == sizeof(float);
    UINT alignedSize = static_cast<UINT>(size);
    if (isConstant)
    {
      constexpr UINT kAlignment = 16;
      alignedSize = ((alignedSize + (kAlignment - 1)) / kAlignment) * kAlignment;
    }
    desc.ByteWidth = alignedSize;
    desc.BindFlags = isConstant ? D3D11_BIND_CONSTANT_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    desc.Usage = data ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = data ? 0 : D3D11_CPU_ACCESS_WRITE;
    desc.StructureByteStride = isConstant ? 0u : static_cast<UINT>(stride);

    D3D11_SUBRESOURCE_DATA initData = { };
    if (data)
      initData.pSysMem = data;

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, data ? &initData : nullptr, &buffer)))
      return { };

    auto* wrapped = new D3D11Buffer(
      m_immediateContext.Get(), buffer.Get(), desc.ByteWidth, desc.BindFlags, desc.Usage);
    return nsObjectPtr<APHIBuffer>(wrapped);
  }

  nsObjectPtr<APHIBuffer> D3D11Context::CreateIndexBuffer(
    const void* data, size_t size, const nsString&, const nsVariantDictionary&)
  {
    if (!m_device || size == 0)
      return { };

    D3D11_BUFFER_DESC desc = { };
    desc.ByteWidth = static_cast<UINT>(size);
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.Usage = data ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = data ? 0 : D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA initData = { };
    if (data)
      initData.pSysMem = data;

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, data ? &initData : nullptr, &buffer)))
      return { };

    auto* wrapped = new D3D11Buffer(
      m_immediateContext.Get(), buffer.Get(), desc.ByteWidth, desc.BindFlags, desc.Usage);
    return nsObjectPtr<APHIBuffer>(wrapped);
  }

  void D3D11Context::UpdateBuffer(
    APHIBuffer* buffer, const void* data, size_t size, size_t)
  {
    if (!buffer || !data || size == 0)
      return;

    auto* d3dBuffer = static_cast<D3D11Buffer*>(buffer);
    if (!d3dBuffer->GetBuffer())
      return;

    if (d3dBuffer->GetD3DUsage() == D3D11_USAGE_DYNAMIC)
    {
      void* mapped = d3dBuffer->Map();
      if (mapped)
      {
        std::memcpy(mapped, data, size);
        d3dBuffer->Unmap();
      }
    }
    else
    {
      m_immediateContext->UpdateSubresource(
        d3dBuffer->GetBuffer(), 0, nullptr, data, 0, 0);
    }
  }

  void D3D11Context::SetViewport(const nsRectFloat& rect, float, float)
  {
    if (!m_immediateContext)
      return;

    // Store the unmodified viewport for shader constants so that
    // viewportOrigin/viewportSize remain in the view's local coordinate space.
    m_currentViewport = rect;
    m_viewportDirty = true;

    // Apply screen offset to the D3D11 viewport only.  The shader will still
    // map vertices using the local origin, and D3D11 will place the clip-space
    // output at the offset position on the back-buffer.
    D3D11_VIEWPORT vp = { };
    vp.TopLeftX = rect.x + m_viewportScreenOffsetX;
    vp.TopLeftY = rect.y + m_viewportScreenOffsetY;
    vp.Width = rect.width;
    vp.Height = rect.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_immediateContext->RSSetViewports(1, &vp);
  }

  void D3D11Context::SetScissorRect(const nsRectU32& rect)
  {
    if (!m_immediateContext)
      return;

    const LONG ox = static_cast<LONG>(m_viewportScreenOffsetX);
    const LONG oy = static_cast<LONG>(m_viewportScreenOffsetY);

    D3D11_RECT r;
    r.left = static_cast<LONG>(rect.x) + ox;
    r.top = static_cast<LONG>(rect.y) + oy;
    r.right = static_cast<LONG>(rect.x + rect.width) + ox;
    r.bottom = static_cast<LONG>(rect.y + rect.height) + oy;
    m_immediateContext->RSSetScissorRects(1, &r);
  }

  void D3D11Context::ResetScissorRect()
  {
    // Per-draw overflow clipping sets a scissor for clipped batches; every
    // other batch must paint against the whole surface again.
    SetScissorRect(nsRectU32(0, 0, m_width, m_height));
  }

  void D3D11Context::SetViewportScreenOffset(float x, float y)
  {
    m_viewportScreenOffsetX = x;
    m_viewportScreenOffsetY = y;
  }

  D3D11_BLEND D3D11Context::StringToBlendFactor(const nsString& value) const
  {
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "Zero"))
      return D3D11_BLEND_ZERO;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "One"))
      return D3D11_BLEND_ONE;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "SrcAlpha"))
      return D3D11_BLEND_SRC_ALPHA;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "InvSrcAlpha"))
      return D3D11_BLEND_INV_SRC_ALPHA;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "DstAlpha"))
      return D3D11_BLEND_DEST_ALPHA;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "InvDstAlpha"))
      return D3D11_BLEND_INV_DEST_ALPHA;
    // Color-channel factors used by separable mix-blend-modes (Multiply,
    // Screen) — these read the destination/source COLOR, which the prior
    // mapping lacked, so those modes couldn't be expressed in fixed-function.
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "DestColor"))
      return D3D11_BLEND_DEST_COLOR;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "InvDestColor"))
      return D3D11_BLEND_INV_DEST_COLOR;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "SrcColor"))
      return D3D11_BLEND_SRC_COLOR;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "InvSrcColor"))
      return D3D11_BLEND_INV_SRC_COLOR;
    return D3D11_BLEND_ONE;
  }

  D3D11_BLEND_OP D3D11Context::StringToBlendOp(const nsString& value) const
  {
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "Add"))
      return D3D11_BLEND_OP_ADD;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "Subtract"))
      return D3D11_BLEND_OP_SUBTRACT;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "RevSubtract"))
      return D3D11_BLEND_OP_REV_SUBTRACT;
    // Min/Max color ops implement the Darken / Lighten separable blend
    // modes exactly (the alpha channel keeps standard over-compositing,
    // set independently in SetBlendState).
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "Min"))
      return D3D11_BLEND_OP_MIN;
    if (nsStringUtils::IsEqual_NoCase(value.GetData(), "Max"))
      return D3D11_BLEND_OP_MAX;
    return D3D11_BLEND_OP_ADD;
  }

  void D3D11Context::SetBlendState(bool enableBlend, const nsString& srcBlend,
    const nsString& dstBlend, const nsString& blendOp)
  {
    if (!m_immediateContext)
      return;

    BlendKey key;
    key.enable = enableBlend;
    key.src = StringToBlendFactor(srcBlend);
    key.dst = StringToBlendFactor(dstBlend);
    key.op = StringToBlendOp(blendOp);

    if (m_hasActiveBlendState && key == m_activeBlendKey)
      return;

    auto it = m_blendStates.find(key);
    if (it == m_blendStates.end())
    {
      D3D11_BLEND_DESC desc = { };
      desc.AlphaToCoverageEnable = FALSE;
      desc.IndependentBlendEnable = FALSE;
      auto& rt = desc.RenderTarget[0];
      rt.BlendEnable = key.enable ? TRUE : FALSE;
      rt.SrcBlend = key.src;
      rt.DestBlend = key.dst;
      rt.BlendOp = key.op;
      // Alpha channel uses pre-multiplied convention: src_alpha * 1 + dst_alpha * (1 -
      // src_alpha). Using key.src (SrcAlpha) here would square the alpha during
      // compositing, causing semi-transparent layers to appear darker than intended.
      rt.SrcBlendAlpha = D3D11_BLEND_ONE;
      rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

      ComPtr<ID3D11BlendState> state;
      if (FAILED(m_device->CreateBlendState(&desc, &state)))
        return;

      it = m_blendStates.emplace(key, state).first;
    }

    float blendFactor[4] = { 0, 0, 0, 0 };
    m_immediateContext->OMSetBlendState(it->second.Get(), blendFactor, 0xffffffff);

    m_activeBlendKey = key;
    m_hasActiveBlendState = true;
  }

  void D3D11Context::SetConstantBuffer(nsUInt32 slot, APHIBuffer* buffer, size_t, size_t)
  {
    if (!m_immediateContext || !buffer)
      return;

    auto* d3dBuffer = static_cast<D3D11Buffer*>(buffer);
    ID3D11Buffer* native = d3dBuffer->GetBuffer();
    if (!native)
      return;

    // Bind to both VS and PS as different cbuffers are used by different shader stages
    // - Slot 0 (GradientBuffer): PS only
    // - Slot 1 (ViewportBuffer): VS and PS
    // - Slot 2 (AtlasStateBuffer): PS only
    // - Slot 3 (TransformBuffer): VS only (for vertex transformation)
    // - Slot 4 (FilterBuffer): PS only
    if (slot == 3)
    {
      // TransformBuffer is used by vertex shader
      m_immediateContext->VSSetConstantBuffers(slot, 1, &native);
    }
    else
    {
      m_immediateContext->PSSetConstantBuffers(slot, 1, &native);
    }
  }

  void D3D11Context::SetVertexBuffer(
    nsUInt32 slot, APHIBuffer* buffer, size_t offset, size_t stride)
  {
    if (!m_immediateContext || !buffer)
      return;

    auto* d3dBuffer = static_cast<D3D11Buffer*>(buffer);
    ID3D11Buffer* native = d3dBuffer->GetBuffer();
    if (!native)
      return;

    UINT s = static_cast<UINT>(stride ? stride : sizeof(HarrlowVG::Vertex));
    UINT offs = static_cast<UINT>(offset);
    m_immediateContext->IASetVertexBuffers(slot, 1, &native, &s, &offs);
  }

  void D3D11Context::SetIndexBuffer(APHIBuffer* buffer, size_t offset)
  {
    if (!m_immediateContext || !buffer)
      return;

    auto* d3dBuffer = static_cast<D3D11Buffer*>(buffer);
    ID3D11Buffer* native = d3dBuffer->GetBuffer();
    if (!native)
      return;

    m_immediateContext->IASetIndexBuffer(
      native, DXGI_FORMAT_R16_UINT, static_cast<UINT>(offset));
  }

  void D3D11Context::UpdateAtlasStateConstant(bool isColorAtlas)
  {
    if (!m_immediateContext || !m_atlasStateBuffer)
      return;

    D3D11_MAPPED_SUBRESOURCE mapped = { };
    if (FAILED(m_immediateContext->Map(
          m_atlasStateBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return;

    float* data = static_cast<float*>(mapped.pData);
    data[0] = isColorAtlas ? 1.0f : 0.0f;
    data[1] = 0.0f; // isVideoYUV
    data[2] = 0.0f; // isSDFText
    data[3] = 0.0f; // sdfScale
    data[4] = 0.0f; // isSDFBorder
    data[5] = 0.0f; // isSDFBoxShadow
    data[6] = 0.0f; // isSDFBackground
    data[7] = 0.0f; // isSDFShape
    m_immediateContext->Unmap(m_atlasStateBuffer.Get(), 0);
    m_cachedAtlasState = data[0];
  }

  void D3D11Context::SetTexture(nsUInt32 slot, APHITexture* texture, APHISampler*)
  {
    if (!m_immediateContext)
      return;

    ID3D11ShaderResourceView* srv = m_whiteTextureSRV.Get();
    bool isColorAtlas = false;
    if (texture)
    {
      auto* d3dTexture = static_cast<D3D11Texture*>(texture);
      if (d3dTexture && d3dTexture->GetSRV())
      {
        srv = d3dTexture->GetSRV();
        isColorAtlas = d3dTexture->IsColorGlyphAtlas();
      }
    }
    UpdateAtlasStateConstant(isColorAtlas);

    m_immediateContext->PSSetShaderResources(slot, 1, &srv);

    ID3D11SamplerState* sampler = m_linearClampSampler.Get();
    if (sampler)
      m_immediateContext->PSSetSamplers(slot, 1, &sampler);
  }

  void D3D11Context::Draw(
    nsUInt32 vertexCount, nsUInt32 startVertex, nsUInt32 instanceCount)
  {
    if (!m_immediateContext)
      return;

    EnsurePipelineState();
    UpdateViewportConstants();
    BindProgramForDraw();
    m_immediateContext->DrawInstanced(vertexCount, instanceCount, startVertex, 0);
  }

  void D3D11Context::BindProgramForDraw()
  {
    const bool wantRec = m_drawRecordsActive;
    if (wantRec == m_recProgramBound)
      return;
    if (wantRec)
    {
      m_immediateContext->IASetInputLayout(m_inputLayoutRec.Get());
      m_immediateContext->VSSetShader(m_vertexShaderRec.Get(), nullptr, 0);
      m_immediateContext->PSSetShader(m_pixelShaderRec.Get(), nullptr, 0);
    }
    else
    {
      m_immediateContext->IASetInputLayout(m_inputLayout.Get());
      m_immediateContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
      m_immediateContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    }
    m_recProgramBound = wantRec;
  }

  void D3D11Context::SetDrawRecords(const void* records, size_t bytes, uint32_t stride)
  {
    if (!m_immediateContext || !m_device || !m_drawRecordsSupported || bytes == 0
      || stride == 0)
      return;
    if (bytes > m_recordCapacity)
    {
      // Structured buffers need ByteWidth to be a whole number of elements.
      size_t count = 16;
      while (count * stride < bytes)
        count *= 2;
      const size_t cap = count * stride;
      m_recordSRV.Reset();
      m_recordBuffer.Reset();
      D3D11_BUFFER_DESC bd = { };
      bd.ByteWidth = static_cast<UINT>(cap);
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = stride;
      if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_recordBuffer)))
        return;
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = { };
      sd.Format = DXGI_FORMAT_UNKNOWN;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.FirstElement = 0;
      sd.Buffer.NumElements = static_cast<UINT>(count);
      if (FAILED(
            m_device->CreateShaderResourceView(m_recordBuffer.Get(), &sd, &m_recordSRV)))
        return;
      m_recordCapacity = cap;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = { };
    if (FAILED(m_immediateContext->Map(
          m_recordBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return;
    std::memcpy(mapped.pData, records, bytes);
    m_immediateContext->Unmap(m_recordBuffer.Get(), 0);
    ID3D11ShaderResourceView* srv = m_recordSRV.Get();
    m_immediateContext->VSSetShaderResources(9, 1, &srv);
    m_immediateContext->PSSetShaderResources(9, 1, &srv);
    ID3D11SamplerState* sampler = m_linearClampSampler.Get();
    if (sampler)
      m_immediateContext->PSSetSamplers(0, 1, &sampler);
    m_drawRecordsActive = true;
  }

  void D3D11Context::BindTextureTable(const std::vector<APHITexture*>& textures)
  {
    if (!m_immediateContext)
      return;
    ID3D11ShaderResourceView* srvs[kDrawRecordTextureSlots] = { };
    for (size_t i = 0; i < kDrawRecordTextureSlots; ++i)
    {
      srvs[i] = m_whiteTextureSRV.Get();
      if (i < textures.size() && textures[i])
        if (auto* d3dTexture = static_cast<D3D11Texture*>(textures[i]);
          d3dTexture->GetSRV())
          srvs[i] = d3dTexture->GetSRV();
    }
    if (m_tableValid && std::memcmp(srvs, m_boundTable, sizeof(srvs)) == 0)
      return;
    std::memcpy(m_boundTable, srvs, sizeof(srvs));
    m_tableValid = true;
    m_immediateContext->PSSetShaderResources(10, kDrawRecordTextureSlots, srvs);
  }

  void D3D11Context::DrawIndexed(
    nsUInt32 indexCount, nsUInt32 startIndex, nsInt32 baseVertex, nsUInt32 instanceCount)
  {
    if (!m_immediateContext)
      return;

    EnsurePipelineState();
    UpdateViewportConstants();
    BindProgramForDraw();

    m_immediateContext->DrawIndexedInstanced(
      indexCount, instanceCount, startIndex, baseVertex, 0);
  }

  void D3D11Context::SubmitCommands()
  {
    if (!m_immediateContext)
      return;

    if (!m_pendingCommandList.commands.empty())
    {
      // Ensure RTV is bound before drawing
      if (m_renderTarget)
      {
        ID3D11RenderTargetView* rtv = m_renderTarget.Get();
        m_immediateContext->OMSetRenderTargets(1, &rtv, nullptr);
      }

      // Bind shaders and default constant buffers BEFORE the batch loop.
      // Per-batch code in SimpleAPHIRenderContext overwrites each CB slot
      // (gradient, atlas, transform, filter) before every DrawIndexed call.
      // If we let EnsurePipelineState run lazily from DrawIndexed, the first
      // batch's per-batch CB bindings get overwritten with identity defaults.
      EnsurePipelineState();

#if NS_ENABLED(APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING)
      // Upload vector glyph curve StructuredBuffer if dirty
      if (aperture::html::FontEngineBridge::IsVectorGlyphRenderingEnabled())
      {
        auto& bridge = aperture::html::FontEngineBridge::Instance();
        auto* fontEngine = bridge.GetFontEngine();
        if (fontEngine)
        {
          auto* curveBuffer = fontEngine->GetVectorGlyphCurveBuffer();
          if (curveBuffer && curveBuffer->GetSegmentCount() > 0)
          {
            const size_t dataSize = static_cast<size_t>(curveBuffer->GetSizeInBytes());
            const nsUInt32 segmentCount = curveBuffer->GetSegmentCount();

            // Recreate buffer if capacity is insufficient
            if (curveBuffer->IsDirty() || dataSize > m_vectorGlyphCurveBufferCapacity)
            {
              m_vectorGlyphCurveSRV.Reset();
              m_vectorGlyphCurveBuffer.Reset();

              D3D11_BUFFER_DESC bufDesc = { };
              bufDesc.ByteWidth = static_cast<UINT>(dataSize);
              bufDesc.Usage = D3D11_USAGE_DEFAULT;
              bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
              bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
              bufDesc.StructureByteStride = 48; // sizeof(nsVectorGlyphGPUCurveSegment)

              D3D11_SUBRESOURCE_DATA initData = { };
              initData.pSysMem = curveBuffer->GetData();

              if (SUCCEEDED(m_device->CreateBuffer(
                    &bufDesc, &initData, &m_vectorGlyphCurveBuffer)))
              {
                m_vectorGlyphCurveBufferCapacity = dataSize;

                // Debug label for RenderDoc
                const char debugName[] = "VectorGlyphCurveBuffer";
                m_vectorGlyphCurveBuffer->SetPrivateData(
                  WKPDID_D3DDebugObjectName, sizeof(debugName) - 1, debugName);

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = segmentCount;
                m_device->CreateShaderResourceView(
                  m_vectorGlyphCurveBuffer.Get(), &srvDesc, &m_vectorGlyphCurveSRV);

                if (m_vectorGlyphCurveSRV)
                {
                  const char srvName[] = "VectorGlyphCurveSRV";
                  m_vectorGlyphCurveSRV->SetPrivateData(
                    WKPDID_D3DDebugObjectName, sizeof(srvName) - 1, srvName);
                }

                nsLog::Info("VectorGlyph D3D11: created StructuredBuffer with {} "
                            "segments ({} bytes)",
                  segmentCount, dataSize);
              }
              else
              {
                nsLog::Error(
                  "VectorGlyph D3D11: CreateBuffer FAILED for {} bytes", dataSize);
              }

              curveBuffer->ResetDirty();
            }

            // Bind at t5
            if (m_vectorGlyphCurveSRV)
            {
              ID3D11ShaderResourceView* srv = m_vectorGlyphCurveSRV.Get();
              m_immediateContext->PSSetShaderResources(5, 1, &srv);
            }

            // Vector glyph band acceleration (t7 directory + t8 index list) — same
            // dirty cycle as the curve buffer; missing/empty band data just
            // leaves the SRVs null → the shader's linear fallback.
            const size_t bandBytes
              = static_cast<size_t>(curveBuffer->GetBandSetSizeInBytes());
            const size_t bandIdxBytes
              = static_cast<size_t>(curveBuffer->GetBandIndexSizeInBytes());
            const nsUInt32 bandSetCount = curveBuffer->GetBandSetCount();
            const nsUInt32 bandIdxCount = curveBuffer->GetBandIndexCount();
            if (bandSetCount > 0 && bandIdxCount > 0
              && (bandBytes != m_vectorGlyphBandBufferCapacity
                || bandIdxBytes != m_vectorGlyphBandIndexBufferCapacity))
            {
              m_vectorGlyphBandSRV.Reset();
              m_vectorGlyphBandBuffer.Reset();
              m_vectorGlyphBandIndexSRV.Reset();
              m_vectorGlyphBandIndexBuffer.Reset();

              D3D11_BUFFER_DESC bd = { };
              bd.ByteWidth = static_cast<UINT>(bandBytes);
              bd.Usage = D3D11_USAGE_DEFAULT;
              bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
              bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
              bd.StructureByteStride = 160; // sizeof(nsVectorGlyphGPUBandSet)
              D3D11_SUBRESOURCE_DATA init = { };
              init.pSysMem = curveBuffer->GetBandSetData();
              if (SUCCEEDED(m_device->CreateBuffer(&bd, &init, &m_vectorGlyphBandBuffer)))
              {
                m_vectorGlyphBandBufferCapacity = bandBytes;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = { };
                sd.Format = DXGI_FORMAT_UNKNOWN;
                sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.NumElements = bandSetCount;
                m_device->CreateShaderResourceView(
                  m_vectorGlyphBandBuffer.Get(), &sd, &m_vectorGlyphBandSRV);
              }

              D3D11_BUFFER_DESC idesc = { };
              idesc.ByteWidth = static_cast<UINT>(bandIdxBytes);
              idesc.Usage = D3D11_USAGE_DEFAULT;
              idesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
              idesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
              idesc.StructureByteStride = 4;
              D3D11_SUBRESOURCE_DATA iinit = { };
              iinit.pSysMem = curveBuffer->GetBandIndexData();
              if (SUCCEEDED(m_device->CreateBuffer(
                    &idesc, &iinit, &m_vectorGlyphBandIndexBuffer)))
              {
                m_vectorGlyphBandIndexBufferCapacity = bandIdxBytes;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = { };
                sd.Format = DXGI_FORMAT_UNKNOWN;
                sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.NumElements = bandIdxCount;
                m_device->CreateShaderResourceView(
                  m_vectorGlyphBandIndexBuffer.Get(), &sd, &m_vectorGlyphBandIndexSRV);
              }
              nsLog::Info("VectorGlyph D3D11: bands for {} glyphs, {} indices",
                bandSetCount, bandIdxCount);
            }
            if (m_vectorGlyphBandSRV && m_vectorGlyphBandIndexSRV)
            {
              ID3D11ShaderResourceView* bandSrvs[2]
                = { m_vectorGlyphBandSRV.Get(), m_vectorGlyphBandIndexSRV.Get() };
              m_immediateContext->PSSetShaderResources(7, 2, bandSrvs);
            }
          }

          // Upload paint command buffer for COLRv1 emoji
          auto* paintEncoder = fontEngine->GetVectorGlyphPaintEncoder();
          if (paintEncoder && paintEncoder->GetCommandCount() > 0)
          {
            const size_t paintDataSize
              = static_cast<size_t>(paintEncoder->GetCommandSizeInBytes());
            const nsUInt32 paintCmdCount = paintEncoder->GetCommandCount();

            if (paintEncoder->IsCommandBufferDirty()
              || paintDataSize > m_vectorGlyphPaintCmdBufferCapacity)
            {
              m_vectorGlyphPaintCmdSRV.Reset();
              m_vectorGlyphPaintCmdBuffer.Reset();

              D3D11_BUFFER_DESC bufDesc = { };
              bufDesc.ByteWidth = static_cast<UINT>(paintDataSize);
              bufDesc.Usage = D3D11_USAGE_DEFAULT;
              bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
              bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
              bufDesc.StructureByteStride
                = 8; // sizeof(nsVectorGlyphPaintElement) = 4 x int16 = 8 bytes

              D3D11_SUBRESOURCE_DATA initData = { };
              initData.pSysMem = paintEncoder->GetCommandData();

              if (SUCCEEDED(m_device->CreateBuffer(
                    &bufDesc, &initData, &m_vectorGlyphPaintCmdBuffer)))
              {
                m_vectorGlyphPaintCmdBufferCapacity = paintDataSize;

                const char debugName[] = "VectorGlyphPaintCmdBuffer";
                m_vectorGlyphPaintCmdBuffer->SetPrivateData(
                  WKPDID_D3DDebugObjectName, sizeof(debugName) - 1, debugName);

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = paintCmdCount;
                m_device->CreateShaderResourceView(
                  m_vectorGlyphPaintCmdBuffer.Get(), &srvDesc, &m_vectorGlyphPaintCmdSRV);

                nsLog::Info("VectorGlyph D3D11: created paint command buffer with {} "
                            "elements ({} bytes)",
                  paintCmdCount, paintDataSize);
              }

              paintEncoder->ResetCommandBufferDirty();
            }

            // Bind at t6
            if (m_vectorGlyphPaintCmdSRV)
            {
              ID3D11ShaderResourceView* srv = m_vectorGlyphPaintCmdSRV.Get();
              m_immediateContext->PSSetShaderResources(6, 1, &srv);
            }
          }
        }
      }
#endif

      m_drawRecordsActive = false; // SetDrawRecords re-arms it on the record path
      m_tableValid = false;
      SimpleAPHIRenderContext::SubmitHarrlowCommands(m_pendingCommandList);
      m_pendingCommandList.Clear();
    }

    m_immediateContext->Flush();
  }

  void D3D11Context::WaitForGPU()
  {
    if (!m_immediateContext)
      return;

    ComPtr<ID3D11Query> query;
    D3D11_QUERY_DESC desc = { };
    desc.Query = D3D11_QUERY_EVENT;
    if (FAILED(m_device->CreateQuery(&desc, &query)))
      return;

    m_immediateContext->End(query.Get());
    while (m_immediateContext->GetData(query.Get(), nullptr, 0, 0) == S_FALSE)
    {
      Sleep(0);
    }
  }

  void D3D11Context::SubmitHarrlowCommands(const HarrlowCommandList& commandList)
  {
    // ── Engine-internal layer-RT submissions must execute IMMEDIATELY ──
    // CompositeLayers/SaveLayerAsTexture render a group's content into a
    // scratch RT mid-frame and then queue a composite quad in the PARENT
    // list that samples the RT later this same frame. Deferring these into
    // m_pendingCommandList meant only the LAST layer list survived (each
    // overwrote the previous) and it executed AFTER the composites — so
    // every opacity/filter/blend group sampled an empty RT and vanished
    // from the live player. Only the app's MAIN list (targetTexture ==
    // null, via RenderCommands) defers so SubmitCommands() can run atlas
    // and vector glyph buffer prep before executing it.
    if (commandList.targetTexture != nullptr)
    {
      m_pipelineReady = false;
      m_hasActiveBlendState = false;
      m_activeBlendKey = { };
      m_drawRecordsActive = false; // SetDrawRecords re-arms it on the record path
      m_tableValid = false;
      SimpleAPHIRenderContext::SubmitHarrlowCommands(commandList);
      // Force full state re-setup for whatever executes next.
      m_pipelineReady = false;
      m_hasActiveBlendState = false;
      return;
    }

    m_pipelineReady = false;
    m_hasActiveBlendState = false;
    m_activeBlendKey = { };
    m_pendingCommandList = commandList;
  }

  void D3D11Context::BindTargetTexture(
    APHITexture* texture, const HarrlowVG::Vec2& size, uintptr_t viewTag)
  {
    NS_UNUSED(viewTag);

    float targetWidth = size.x > 0.0f ? size.x : static_cast<float>(m_width);
    float targetHeight = size.y > 0.0f ? size.y : static_cast<float>(m_height);

    ID3D11RenderTargetView* rtv = nullptr;
    m_externalRenderTarget.Reset();
    if (texture)
    {
      auto* srvHandle
        = static_cast<ID3D11ShaderResourceView*>(texture->GetNativeHandle());
      if (srvHandle && m_device)
      {
        ComPtr<ID3D11Resource> resource;
        srvHandle->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture2D;
        if (resource && SUCCEEDED(resource.As(&texture2D)))
        {
          D3D11_TEXTURE2D_DESC desc = { };
          texture2D->GetDesc(&desc);
          if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0)
          {
            if (SUCCEEDED(m_device->CreateRenderTargetView(
                  texture2D.Get(), nullptr, &m_externalRenderTarget)))
            {
              rtv = m_externalRenderTarget.Get();
            }
            else
            {
              nsLog::Warning("D3D11Context: BindTargetTexture failed to create RTV for "
                             "provided texture; using default render target.");
            }
          }
          else
          {
            nsLog::Warning("D3D11Context: BindTargetTexture received texture without "
                           "render-target bind flag; using default render target.");
          }
        }
        else
        {
          nsLog::Warning(
            "D3D11Context: BindTargetTexture could not resolve ID3D11Texture2D from "
            "provided texture; using default render target.");
        }
      }
      else
      {
        nsLog::Warning("D3D11Context: BindTargetTexture received texture without D3D11 "
                       "SRV handle; using default render target.");
      }
    }

    // A freshly bound LAYER target must start fully transparent: scratch RTs
    // are pooled and reused across layers and frames, so without this clear
    // the RT accumulates every previous layer's pixels and each composite
    // quad samples STALE content at its own rect (live symptom: JS-animated
    // opacity groups froze at their first-frame content — opacity still
    // animated because it rides the composite quad, but transforms appeared
    // dead). Only the external layer RT is cleared — never the backbuffer —
    // and not when a layer flushed mid-record (backdrop-filter) continues.
    if (rtv && m_immediateContext && !BoundLayerTargetKeepsContents())
    {
      const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      m_immediateContext->ClearRenderTargetView(rtv, clear);
    }

    if (!rtv && m_renderToTexture && m_rttRTV)
      rtv = m_rttRTV.Get();
    if (!rtv && m_renderTarget)
      rtv = m_renderTarget.Get();

    if (rtv && m_immediateContext)
    {
      m_immediateContext->OMSetRenderTargets(1, &rtv, nullptr);
    }

    HarrlowVG::Vec2 viewportSize(targetWidth, targetHeight);
    SimpleAPHIRenderContext::BindTargetTexture(texture, viewportSize, viewTag);
    m_pipelineReady = false;
  }

  void D3D11Context::ReleaseTargetTexture(APHITexture* texture)
  {
    m_externalRenderTarget.Reset();
    // Restore the appropriate render target
    ID3D11RenderTargetView* restoreRTV = nullptr;
    if (m_renderToTexture && m_rttRTV)
      restoreRTV = m_rttRTV.Get();
    else if (m_renderTarget)
      restoreRTV = m_renderTarget.Get();
    if (m_immediateContext && restoreRTV)
    {
      m_immediateContext->OMSetRenderTargets(1, &restoreRTV, nullptr);
    }
    SimpleAPHIRenderContext::ReleaseTargetTexture(texture);
    nsRectFloat viewport(
      0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    SetViewport(viewport, 0.0f, 1.0f);
    SetScissorRect(nsRectU32(0, 0, m_width, m_height));
    m_pipelineReady = false;
  }

  aperture::harrlow::renderpath::APHITexture* D3D11Context::PrepareAtlasTexture(
    HarrlowVG::Atlas* atlas)
  {
    if (!atlas || !m_device || !m_immediateContext)
      return nullptr;

    auto& entry = m_atlasTextures[atlas];
    if (!entry.texture.isValid())
      entry.texture.reset(new D3D11Texture());

    const HarrlowVG::Vec2ui size = atlas->GetSize();
    const UINT width = size.x;
    const UINT height = size.y;

    if (width == 0 || height == 0)
      return nullptr;

    const bool isColor = atlas->IsColor();
    const DXGI_FORMAT desiredFormat
      = isColor ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;

    bool needsInit = entry.texture.rawPtr() == nullptr
      || entry.texture->GetWidth() != width || entry.texture->GetHeight() != height
      || entry.texture->GetFormatDXGI() != desiredFormat;

    if (needsInit)
    {
      if (!entry.texture->Initialize(m_device.Get(), width, height, desiredFormat))
        return nullptr;
      entry.dirty = true;
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG)
      nsLog::Info("Atlas (D3D11): full-update realloc, atlas_ptr={0}, new size={1}x{2}, "
                  "isColor={3}",
        reinterpret_cast<uintptr_t>(atlas), width, height, isColor);
#endif
    }

    if (entry.texture)
      entry.texture->SetIsColorGlyphAtlas(isColor);

    if (entry.dirty)
    {
      const void* data = atlas->GetData();
      if (!data)
        return entry.texture.rawPtr();

      const UINT stride = atlas->GetStride();
      const UINT bytesPerPixel = isColor ? 4u : 1u;
      const UINT rowPitch = stride ? stride : width * bytesPerPixel;

      // Incremental sub-rect upload.  When the atlas grew (needsInit)
      // the new GPU texture has no contents, so we must upload the whole
      // buffer.  Otherwise, if we accumulated a dirty-rect union from the
      // glyph-add updates this frame, push only that box — for a 2048x8192
      // page this turns a ~64 MB UpdateSubresource into a few KB.
      if (!needsInit && entry.hasDirtyRegion)
      {
        int L = entry.dirtyL < 0 ? 0 : entry.dirtyL;
        int T = entry.dirtyT < 0 ? 0 : entry.dirtyT;
        int R = entry.dirtyR > static_cast<int>(width) ? static_cast<int>(width)
                                                       : entry.dirtyR;
        int B = entry.dirtyB > static_cast<int>(height) ? static_cast<int>(height)
                                                        : entry.dirtyB;
        if (R > L && B > T)
        {
          const auto* src = static_cast<const uint8_t*>(data)
            + static_cast<size_t>(T) * rowPitch + static_cast<size_t>(L) * bytesPerPixel;
          entry.texture->UploadRegion(m_immediateContext.Get(), src, rowPitch,
            static_cast<UINT>(L), static_cast<UINT>(T), static_cast<UINT>(R - L),
            static_cast<UINT>(B - T));
        }
        else
        {
          entry.texture->Upload(m_immediateContext.Get(), data, rowPitch);
        }
      }
      else
      {
        entry.texture->Upload(m_immediateContext.Get(), data, rowPitch);
      }

      entry.dirty = false;
      entry.hasDirtyRegion = false; // consumed — reset the accumulator
    }

    return entry.texture.rawPtr();
  }

  aperture::harrlow::renderpath::APHITexture* D3D11Context::PrepareCanvasTexture(
    uint64_t handle, const CanvasSurfaceView& surface)
  {
    if (!m_device || !m_immediateContext)
      return nullptr;

    if (!surface.pixels || surface.width == 0 || surface.height == 0)
      return nullptr;

    constexpr DXGI_FORMAT kCanvasFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto& entry = m_canvasTextures[handle];
    const bool needsRecreate = !entry.texture.isValid()
      || entry.texture->GetWidth() != surface.width
      || entry.texture->GetHeight() != surface.height
      || entry.texture->GetFormatDXGI() != kCanvasFormat;

    if (needsRecreate)
    {
      entry.texture.reset();
      entry.texture.reset(new D3D11Texture());
      if (!entry.texture->Initialize(
            m_device.Get(), surface.width, surface.height, kCanvasFormat))
      {
        m_canvasTextures.erase(handle);
        return nullptr;
      }
      entry.uploadedGeneration = 0;
    }

    if (entry.texture)
      entry.texture->SetIsColorGlyphAtlas(false);

    entry.lastUsedFrame = m_surfaceFrameIndex;

    if (entry.uploadedGeneration != surface.generation || surface.dirty)
    {
      // Large first image uploads arrive a band of rows per frame; the texture
      // is not drawn until the last band is in (SimpleAPHIRenderContext).
      const UINT stride = surface.stride ? surface.stride : surface.width * 4u;
      uint32_t rowBegin = 0, rowEnd = surface.height;
      const bool complete = PlanSurfaceUpload(handle, surface, rowBegin, rowEnd);
      if (rowBegin == 0 && rowEnd == surface.height)
        entry.texture->Upload(m_immediateContext.Get(), surface.pixels, stride);
      else if (rowEnd > rowBegin)
        entry.texture->UploadRegion(m_immediateContext.Get(),
          surface.pixels + static_cast<size_t>(rowBegin) * stride, stride, 0, rowBegin,
          surface.width, rowEnd - rowBegin);
      if (!complete)
        return nullptr;
      entry.uploadedGeneration = surface.generation;
      // The handle is opaque here: clear the owning bridge's dirty flag by
      // notifying every CanvasSurfaceView producer (each ignores handles it
      // does not own). Notifying the canvas bridge alone left decoded images
      // dirty forever, so every image re-uploaded on every frame.
      NotifyCanvasUploadComplete(handle, surface.generation);
      NotifySVGUploadComplete(handle, surface.generation);
      NotifyLottieUploadComplete(handle, surface.generation);
      NotifyImageUploadComplete(handle, surface.generation);
    }

    return entry.texture.rawPtr();
  }

  aperture::harrlow::renderpath::APHITexture* D3D11Context::UploadCanvasSurface(
    uint64_t handle, const CanvasSurfaceView& surface)
  {
    return PrepareCanvasTexture(handle, surface);
  }

  aperture::harrlow::renderpath::APHITexture* D3D11Context::UploadVideoSurface(
    uint64_t handle, const VideoSurfaceView& surface)
  {
    if (!surface.yPlane || !surface.uPlane || !surface.vPlane)
      return nullptr;

    if (surface.width == 0 || surface.height == 0)
      return nullptr;

    constexpr DXGI_FORMAT kVideoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto& entry = m_videoTextures[handle];
    const bool needsRecreate = !entry.texture.isValid()
      || entry.texture->GetWidth() != surface.width
      || entry.texture->GetHeight() != surface.height
      || entry.texture->GetFormatDXGI() != kVideoFormat;

    if (needsRecreate)
    {
      entry.texture.reset();
      entry.texture.reset(new D3D11Texture());
      if (!entry.texture->Initialize(
            m_device.Get(), surface.width, surface.height, kVideoFormat))
      {
        m_videoTextures.erase(handle);
        return nullptr;
      }
      entry.uploadedGeneration = UINT64_MAX; // sentinel: force first upload
      // Allocate RGBA buffer for YUV conversion
      entry.rgbaBuffer.resize(static_cast<size_t>(surface.width) * surface.height * 4);
    }

    if (entry.texture)
      entry.texture->SetIsColorGlyphAtlas(false);

    entry.lastUsedFrame = m_surfaceFrameIndex;

    if (entry.uploadedGeneration != surface.generation || surface.dirty)
    {
      // Convert YUV420 to RGBA using the helper
      ConvertYUV420ToRGBA(surface, entry.rgbaBuffer.data());

      // Upload converted RGBA data
      const uint32_t rowPitch = surface.width * 4;
      entry.texture->Upload(m_immediateContext.Get(), entry.rgbaBuffer.data(), rowPitch);
      entry.uploadedGeneration = surface.generation;
      NotifyVideoUploadComplete(handle, surface.generation);
    }

    return entry.texture.rawPtr();
  }

  aperture::harrlow::renderpath::APHITexture* D3D11Context::ResolveExternalTextureHandle(
    HarrlowVG::TextureHandle handle)
  {
    if (!handle)
      return nullptr;

    auto* atlas = static_cast<HarrlowVG::Atlas*>(handle);
    auto* texture = PrepareAtlasTexture(atlas);

    if (texture)
      return texture;

    return SimpleAPHIRenderContext::ResolveExternalTextureHandle(handle);
  }

  void D3D11Context::OnAtlasUpdated(HarrlowVG::Atlas* atlas)
  {
    if (!atlas)
      return;

    AtlasEntry& entry = m_atlasTextures[atlas];
    entry.dirty = true;

    // Accumulate the UNION of every update's dirty sub-rect between
    // draws.  This callback can fire several times for one atlas in a single
    // frame (multiple text runs introducing new glyphs), each carrying its
    // own rect; uploading only the last would drop the earlier glyphs.  A
    // full/resize update (no sub-rect) forces a whole-texture upload.
    if (atlas->HasDirtyRect())
    {
      const int x = atlas->GetDirtyX();
      const int y = atlas->GetDirtyY();
      const int r = x + atlas->GetDirtyWidth();
      const int b = y + atlas->GetDirtyHeight();
      if (!entry.hasDirtyRegion)
      {
        entry.dirtyL = x;
        entry.dirtyT = y;
        entry.dirtyR = r;
        entry.dirtyB = b;
        entry.hasDirtyRegion = true;
      }
      else
      {
        entry.dirtyL = (x < entry.dirtyL) ? x : entry.dirtyL;
        entry.dirtyT = (y < entry.dirtyT) ? y : entry.dirtyT;
        entry.dirtyR = (r > entry.dirtyR) ? r : entry.dirtyR;
        entry.dirtyB = (b > entry.dirtyB) ? b : entry.dirtyB;
      }
    }
    else
    {
      // Whole-texture update requested — discard any accumulated sub-rect.
      entry.hasDirtyRegion = false;
    }
  }

  void D3D11Context::SetMSAASampleCount(nsGALMSAASampleCount::Enum count)
  {
    if (count == m_msaaSampleCount)
      return;
    m_msaaSampleCount = count;
    ReleaseMSAAResources();
    if (count != nsGALMSAASampleCount::None && m_device)
      CreateMSAAResources();
  }

  bool D3D11Context::CreateMSAAResources()
  {
    if (!m_device || m_width == 0 || m_height == 0)
      return false;
    if (m_msaaSampleCount == nsGALMSAASampleCount::None)
      return false;

    UINT sampleCount = static_cast<UINT>(m_msaaSampleCount);
    UINT qualityLevels = 0;
    if (FAILED(m_device->CheckMultisampleQualityLevels(
          DXGI_FORMAT_R8G8B8A8_UNORM, sampleCount, &qualityLevels))
      || qualityLevels == 0)
    {
      nsLog::Warning("D3D11Context: MSAA {0}x not supported", sampleCount);
      m_msaaSampleCount = nsGALMSAASampleCount::None;
      return false;
    }

    // MSAA color texture
    D3D11_TEXTURE2D_DESC colorDesc = { };
    colorDesc.Width = m_width;
    colorDesc.Height = m_height;
    colorDesc.MipLevels = 1;
    colorDesc.ArraySize = 1;
    colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorDesc.SampleDesc.Count = sampleCount;
    colorDesc.SampleDesc.Quality = 0;
    colorDesc.Usage = D3D11_USAGE_DEFAULT;
    colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    if (FAILED(m_device->CreateTexture2D(&colorDesc, nullptr, &m_msaaColorTexture)))
    {
      nsLog::Error("D3D11Context: Failed to create MSAA color texture");
      return false;
    }

    if (FAILED(m_device->CreateRenderTargetView(
          m_msaaColorTexture.Get(), nullptr, &m_msaaRTV)))
    {
      nsLog::Error("D3D11Context: Failed to create MSAA RTV");
      ReleaseMSAAResources();
      return false;
    }

    // MSAA depth-stencil texture
    D3D11_TEXTURE2D_DESC depthDesc = { };
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = sampleCount;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    if (FAILED(m_device->CreateTexture2D(&depthDesc, nullptr, &m_msaaDepthTexture)))
    {
      nsLog::Error("D3D11Context: Failed to create MSAA depth texture");
      ReleaseMSAAResources();
      return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = { };
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;

    if (FAILED(m_device->CreateDepthStencilView(
          m_msaaDepthTexture.Get(), &dsvDesc, &m_msaaDSV)))
    {
      nsLog::Error("D3D11Context: Failed to create MSAA DSV");
      ReleaseMSAAResources();
      return false;
    }

    nsLog::Info("D3D11Context: Created MSAA {0}x resources ({1}x{2})", sampleCount,
      m_width, m_height);
    return true;
  }

  void D3D11Context::ReleaseMSAAResources()
  {
    m_msaaDSV.Reset();
    m_msaaDepthTexture.Reset();
    m_msaaRTV.Reset();
    m_msaaColorTexture.Reset();
  }

  void D3D11Context::SetRenderToTexture(bool enable)
  {
    IPlayerRenderContext::SetRenderToTexture(enable);
    if (enable)
    {
      if (!CreateRTTResources())
        nsLog::Error("D3D11Context: Failed to create RTT resources");
    }
    else
    {
      ReleaseRTTResources();
    }
  }

  bool D3D11Context::CreateRTTResources()
  {
    ReleaseRTTResources();

    if (!m_device || m_width == 0 || m_height == 0)
      return false;

    D3D11_TEXTURE2D_DESC texDesc = { };
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_rttTexture)))
      return false;

    if (FAILED(m_device->CreateRenderTargetView(m_rttTexture.Get(), nullptr, &m_rttRTV)))
    {
      m_rttTexture.Reset();
      return false;
    }

    if (FAILED(
          m_device->CreateShaderResourceView(m_rttTexture.Get(), nullptr, &m_rttSRV)))
    {
      m_rttRTV.Reset();
      m_rttTexture.Reset();
      return false;
    }

    nsLog::Info(
      "D3D11Context: Created RTT offscreen target ({0}x{1})", m_width, m_height);
    return true;
  }

  void D3D11Context::ReleaseRTTResources()
  {
    m_rttSRV.Reset();
    m_rttRTV.Reset();
    m_rttTexture.Reset();
  }

  void D3D11Context::ResolveTexture(aperture::harrlow::renderpath::APHITexture* msaaSrc,
    aperture::harrlow::renderpath::APHITexture* nonMsaaDst)
  {
    if (!msaaSrc || !nonMsaaDst || !m_immediateContext)
      return;

    auto* srcTex = static_cast<D3D11Texture*>(msaaSrc);
    auto* dstTex = static_cast<D3D11Texture*>(nonMsaaDst);

    if (!srcTex->GetTexture() || !dstTex->GetTexture())
      return;

    m_immediateContext->ResolveSubresource(
      dstTex->GetTexture(), 0, srcTex->GetTexture(), 0, dstTex->GetFormatDXGI());
  }

  void D3D11Context::CopyTexture(aperture::harrlow::renderpath::APHITexture* src,
    aperture::harrlow::renderpath::APHITexture* dst)
  {
    if (!src || !dst || !m_immediateContext)
      return;

    auto* srcTex = static_cast<D3D11Texture*>(src);
    auto* dstTex = static_cast<D3D11Texture*>(dst);
    if (!srcTex->GetTexture() || !dstTex->GetTexture())
      return;

    m_immediateContext->CopyResource(dstTex->GetTexture(), srcTex->GetTexture());
  }

  bool D3D11Context::CaptureBackdropForBlend()
  {
    // Only the render-to-texture path has a readable colour target. (Direct-to-
    // backbuffer and MSAA paths fall back to plain alpha compositing.)
    if (!m_immediateContext || !m_device || !m_renderToTexture || !m_rttTexture)
      return false;

    D3D11_TEXTURE2D_DESC srcDesc = { };
    m_rttTexture->GetDesc(&srcDesc);

    // (Re)create the backdrop texture if missing or size-mismatched.
    if (m_backdropTexture)
    {
      D3D11_TEXTURE2D_DESC bdDesc = { };
      m_backdropTexture->GetDesc(&bdDesc);
      if (bdDesc.Width != srcDesc.Width || bdDesc.Height != srcDesc.Height
        || bdDesc.Format != srcDesc.Format)
      {
        m_backdropSRV.Reset();
        m_backdropTexture.Reset();
      }
    }
    if (!m_backdropTexture)
    {
      D3D11_TEXTURE2D_DESC desc = srcDesc;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.CPUAccessFlags = 0;
      desc.MiscFlags = 0;
      desc.SampleDesc.Count = 1;
      desc.SampleDesc.Quality = 0;
      if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_backdropTexture)))
        return false;
      if (FAILED(m_device->CreateShaderResourceView(
            m_backdropTexture.Get(), nullptr, &m_backdropSRV)))
      {
        m_backdropTexture.Reset();
        return false;
      }
    }

    // Snapshot the current RT content (everything drawn so far = the backdrop)
    // and bind it to PS slot 1 (gTextureU) for the in-shader composite. The RTT
    // is only the COPY SOURCE here (still bound as RTV, which is legal for a
    // copy); the bound SRV is the separate backdrop texture, so there is no
    // read/write hazard on the same resource.
    m_immediateContext->CopyResource(m_backdropTexture.Get(), m_rttTexture.Get());
    ID3D11ShaderResourceView* srv = m_backdropSRV.Get();
    m_immediateContext->PSSetShaderResources(1, 1, &srv);
    return true;
  }

  void D3D11Context::BindFilterComputeShader(
    aperture::harrlow::renderpath::FilterType type)
  {
    if (!m_immediateContext)
      return;

    // The filter runs right after the layer submit, while the layer RT is
    // typically STILL the OM render target. D3D11 silently nulls CS SRV/UAV
    // binds that conflict with the OM — detach it first or the kernel reads
    // zeros (the blur chain then composites a fully transparent layer). The
    // next BindTargetTexture re-establishes whatever OM state is needed.
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_immediateContext->OMSetRenderTargets(1, &nullRTV, nullptr);

    using FT = aperture::harrlow::renderpath::FilterType;
    ID3D11ComputeShader* cs = nullptr;
    switch (type)
    {
      case FT::Blur:
      case FT::BlurH:
        cs = m_blurHComputeShader.Get();
        break;
      case FT::BlurV:
        cs = m_blurVComputeShader.Get();
        break;
      case FT::DropShadow:
        cs = m_dropShadowComputeShader.Get();
        break;
      case FT::DropShadowCompose:
        cs = m_dropShadowComposeComputeShader.Get();
        break;
      case FT::MaskMultiply:
        cs = m_maskMultiplyComputeShader.Get();
        break;
      case FT::ColorMatrix:
        // All color-matrix filters (brightness/contrast/saturate/grayscale/
        // sepia/invert/hue-rotate) share one kernel; the 4x4 is uploaded in
        // the FilterCB (gColorMatrix0..3).
        cs = m_colorMatrixComputeShader.Get();
        break;
      default:
        break;
    }

    m_immediateContext->CSSetShader(cs, nullptr, 0);
  }

  void D3D11Context::DoDispatchCompute(
    uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
  {
    if (!m_immediateContext)
      return;
    m_immediateContext->Dispatch(groupsX, groupsY, groupsZ);
  }

  void D3D11Context::BindComputeOutputUAV(
    aperture::harrlow::renderpath::APHITexture* outputTexture)
  {
    if (!m_immediateContext || !outputTexture)
      return;

    auto* d3dTex = static_cast<D3D11Texture*>(outputTexture);
    ID3D11UnorderedAccessView* uav = d3dTex->GetUAV();
    if (uav)
    {
      m_immediateContext->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    }
  }

  void D3D11Context::BindComputeInputSRV(
    aperture::harrlow::renderpath::APHITexture* inputTexture)
  {
    if (!m_immediateContext || !inputTexture)
      return;

    auto* d3dTex = static_cast<D3D11Texture*>(inputTexture);
    ID3D11ShaderResourceView* srv = d3dTex->GetSRV();
    if (srv)
    {
      m_immediateContext->CSSetShaderResources(0, 1, &srv);
    }

    // Also bind our sampler for compute SampleLevel calls
    ID3D11SamplerState* sampler = m_linearClampSampler.Get();
    if (sampler)
    {
      m_immediateContext->CSSetSamplers(0, 1, &sampler);
    }
  }

  void D3D11Context::BindComputeInputSRV1(
    aperture::harrlow::renderpath::APHITexture* inputTexture)
  {
    if (!m_immediateContext || !inputTexture)
      return;

    auto* d3dTex = static_cast<D3D11Texture*>(inputTexture);
    ID3D11ShaderResourceView* srv = d3dTex->GetSRV();
    if (srv)
    {
      // Bind at register t1 — the secondary input for compose passes.
      m_immediateContext->CSSetShaderResources(1, 1, &srv);
    }
  }

  void D3D11Context::UnbindComputeResources()
  {
    if (!m_immediateContext)
      return;

    // Unbind compute SRV/UAV/CS to prevent resource hazards.  Clear both
    // SRV slots (t0 and the t1 used by compose passes).
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    m_immediateContext->CSSetShaderResources(0, 2, nullSRVs);
    m_immediateContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    m_immediateContext->CSSetShader(nullptr, nullptr, 0);
  }

  void D3D11Context::BindComputeConstantBuffer(
    uint32_t slot, aperture::harrlow::renderpath::APHIBuffer* buffer)
  {
    if (!m_immediateContext || !buffer)
      return;

    auto* d3dBuffer = static_cast<D3D11Buffer*>(buffer);
    ID3D11Buffer* native = d3dBuffer->GetBuffer();
    if (native)
    {
      m_immediateContext->CSSetConstantBuffers(slot, 1, &native);
    }
  }

  void* D3D11Context::GetContentSRV()
  {
    return m_rttSRV.Get();
  }

  void D3D11Context::ResizeContent(nsUInt32 /*width*/, nsUInt32 /*height*/)
  {
    // Intentional no-op. The RTT must stay at the swap-chain (window) dimensions
    // so that the depth/stencil buffer dimensions always match. A mismatch breaks
    // stencil-based clipping and corrupts glyph rendering (missing letters).
    //
    // The UIView logical viewport is set separately via SetViewport(), and the
    // shell crops the displayed UV region so only the content-panel area is shown.
  }

  void D3D11Context::BindBackbuffer(const nsColor& clearColor)
  {
    if (!m_immediateContext || !m_renderTarget)
      return;

    // Switch the OM render target from the offscreen RTT to the swapchain backbuffer so
    // that subsequent ImGui rendering goes to the backbuffer.  EndFrame() will then skip
    // the CopyResource blit (ImGui composites the RTT via ImGui::Image() instead).
    float col[4] = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
    m_immediateContext->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);
    m_immediateContext->ClearRenderTargetView(m_renderTarget.Get(), col);
    m_backbufferBound = true;
  }

} // namespace apui::samples
