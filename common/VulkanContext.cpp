// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
/*
 * VulkanContext.cpp - Vulkan render backend for the samples.
 *
 * See the header for the design. The three rules everything below follows:
 *   1. every byte the engine hands us for this frame goes into this frame's ring;
 *   2. engine-created images live in VK_IMAGE_LAYOUT_GENERAL, transitions are
 *      coarse "everything -> everything" barriers at target switches and copies;
 *   3. rendering is suspended (vkCmdEndRendering) for any copy/dispatch and
 *      resumed lazily by the next draw with LOAD_OP_LOAD.
 */
#ifdef APERTURE_HAS_VULKAN
#  include "VulkanContext.h"

#  include <SDKBuildInfo.h> // APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING

#  include <APHTML/APEngine.h>
#  include <APHTML/Font/FontEngineBridge.h>
#  include <APHTML/Interfaces/APCFileSystem.h>
#  include <APHTML/Interfaces/APCPlatform.h>
#  include <APHarrlow/VectorEngine/Core/Common.hpp>
#  include <APHarrlow/VectorEngine/Core/Text.hpp>
#  include <FontEngine/FontEngine.h>
#  include <FontEngine/VectorGlyphCurveBuffer.h>
#  include <FontEngine/VectorGlyphPaintEncoder.h>
#  include <Foundation/Logging/Log.h>
#  include <Foundation/Strings/StringUtils.h>

#  include <algorithm>
#  include <cstdio>
#  include <cstring>
#  include <iterator>

using namespace aperture::harrlow::renderpath;

namespace apui::samples
{
  // ==========================================================================
  // Resource wrappers
  // ==========================================================================

  class VulkanContext::VulkanBuffer final : public APHIBuffer
  {
  public:
    VulkanBuffer(size_t size, bool isConstant)
      : m_size(size)
      , m_isConstant(isConstant)
    {
      if (isConstant)
        cpu.resize(size);
    }
    size_t GetSize() const override { return m_size; }
    nsVariantDictionary GetUsage() const override { return { }; }
    void* GetNativeHandle() const override
    {
      return reinterpret_cast<void*>(slice.buffer);
    }
    void* Map(size_t, size_t) override { return m_isConstant ? cpu.data() : nullptr; }
    void Unmap() override { }
    bool IsConstant() const { return m_isConstant; }

    /// Constants: copied into the ring per draw. Vertex/index: the last upload,
    /// re-staged when the engine's geometry cache binds this buffer in a later
    /// frame without uploading again (the ring slice below is per-frame).
    std::vector<uint8_t> cpu;
    RingSlice slice; ///< vertex/index data for the frame it was written in
    uint64_t frame = 0;

  private:
    size_t m_size;
    bool m_isConstant;
  };

  class VulkanContext::VulkanTexture final : public APHITexture
  {
  public:
    explicit VulkanTexture(VulkanContext* owner)
      : m_owner(owner)
    {
    }
    ~VulkanTexture() override
    {
      VkImage image = m_image;
      VkDeviceMemory memory = m_memory;
      VkImageView view = m_view;
      VkDevice device = m_owner->m_device;
      if (image != VK_NULL_HANDLE)
      {
        m_owner->DeferDestroy(
          [device, image, memory, view]()
          {
            if (view)
              vkDestroyImageView(device, view, nullptr);
            if (image)
              vkDestroyImage(device, image, nullptr);
            if (memory)
              vkFreeMemory(device, memory, nullptr);
          });
      }
    }

    bool Create(uint32_t width, uint32_t height, VkFormat format)
    {
      VkDevice device = m_owner->m_device;
      m_width = width;
      m_height = height;
      m_format = format;

      VkImageCreateInfo info = { };
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = { width, height, 1 };
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      if (format == VK_FORMAT_R8G8B8A8_UNORM)
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT; // filter compute output
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(device, &info, nullptr, &m_image) != VK_SUCCESS)
        return false;

      VkMemoryRequirements req = { };
      vkGetImageMemoryRequirements(device, m_image, &req);
      VkMemoryAllocateInfo alloc = { };
      alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = m_owner->FindMemoryTypeIndex(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (vkAllocateMemory(device, &alloc, nullptr, &m_memory) != VK_SUCCESS)
        return false;
      vkBindImageMemory(device, m_image, m_memory, 0);

      VkImageViewCreateInfo view = { };
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = m_image;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = format;
      view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      return vkCreateImageView(device, &view, nullptr, &m_view) == VK_SUCCESS;
    }

    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return 1u; }
    uint32_t GetMipLevels() const override { return 1u; }
    nsString GetFormat() const override
    {
      switch (m_format)
      {
        case VK_FORMAT_R8_UNORM:
          return "R8";
        case VK_FORMAT_B8G8R8A8_UNORM:
          return "BGRA8";
        default:
          return "RGBA8";
      }
    }
    nsVariantDictionary GetUsage() const override { return { }; }
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_view); }

    VkImage image() const { return m_image; }
    VkImageView view() const { return m_view; }
    VkFormat format() const { return m_format; }
    uint32_t bytesPerPixel() const { return m_format == VK_FORMAT_R8_UNORM ? 1u : 4u; }

    bool inGeneralLayout = false; ///< first use transitions UNDEFINED -> GENERAL
    bool isColorAtlas = false;

  private:
    VulkanContext* m_owner;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    uint32_t m_width = 0, m_height = 0;
  };

  class VulkanContext::VulkanRenderTarget final : public APHIRenderTarget
  {
  public:
    VulkanRenderTarget(nsObjectPtr<VulkanTexture> color, uint32_t w, uint32_t h)
      : m_color(std::move(color))
      , m_width(w)
      , m_height(h)
    {
    }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    APHITexture* GetColorTexture() const override { return m_color.get(); }
    APHITexture* GetDepthTexture() const override { return nullptr; }
    void* GetNativeHandle() const override
    {
      return m_color ? m_color->GetNativeHandle() : nullptr;
    }

  private:
    nsObjectPtr<VulkanTexture> m_color;
    uint32_t m_width, m_height;
  };

  // ==========================================================================
  // File-scope helpers
  // ==========================================================================

  namespace
  {
    VkFormat FormatFromString(const nsString& s)
    {
      if (nsStringUtils::IsEqual_NoCase(s.GetData(), "R8"))
        return VK_FORMAT_R8_UNORM;
      if (nsStringUtils::IsEqual_NoCase(s.GetData(), "BGRA8"))
        return VK_FORMAT_B8G8R8A8_UNORM;
      return VK_FORMAT_R8G8B8A8_UNORM;
    }

    VkBlendFactor BlendFactorFromString(const nsString& v)
    {
      const char* s = v.GetData();
      if (nsStringUtils::IsEqual_NoCase(s, "Zero"))
        return VK_BLEND_FACTOR_ZERO;
      if (nsStringUtils::IsEqual_NoCase(s, "SrcAlpha"))
        return VK_BLEND_FACTOR_SRC_ALPHA;
      if (nsStringUtils::IsEqual_NoCase(s, "InvSrcAlpha"))
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      if (nsStringUtils::IsEqual_NoCase(s, "DstAlpha"))
        return VK_BLEND_FACTOR_DST_ALPHA;
      if (nsStringUtils::IsEqual_NoCase(s, "InvDstAlpha"))
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      if (nsStringUtils::IsEqual_NoCase(s, "DestColor"))
        return VK_BLEND_FACTOR_DST_COLOR;
      if (nsStringUtils::IsEqual_NoCase(s, "InvDestColor"))
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      if (nsStringUtils::IsEqual_NoCase(s, "SrcColor"))
        return VK_BLEND_FACTOR_SRC_COLOR;
      if (nsStringUtils::IsEqual_NoCase(s, "InvSrcColor"))
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      return VK_BLEND_FACTOR_ONE;
    }

    VkBlendOp BlendOpFromString(const nsString& v)
    {
      const char* s = v.GetData();
      if (nsStringUtils::IsEqual_NoCase(s, "Subtract"))
        return VK_BLEND_OP_SUBTRACT;
      if (nsStringUtils::IsEqual_NoCase(s, "RevSubtract"))
        return VK_BLEND_OP_REVERSE_SUBTRACT;
      if (nsStringUtils::IsEqual_NoCase(s, "Min"))
        return VK_BLEND_OP_MIN;
      if (nsStringUtils::IsEqual_NoCase(s, "Max"))
        return VK_BLEND_OP_MAX;
      return VK_BLEND_OP_ADD;
    }

    /// Engine constant-buffer slot -> shader binding in set 0 (see UIPlayer.hlsl
    /// VK_BIND).
    constexpr uint32_t kSlotToBinding[7] = { 1, 0, 2, 6, 3, 4, 5 };

    constexpr VkDeviceSize kRingChunkBytes = 16u * 1024u * 1024u;

    VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a)
    {
      return (v + a - 1) / a * a;
    }

#  if NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
      if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        nsLog::Error("Vulkan: {0}", data->pMessage);
      else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        nsLog::Warning("Vulkan: {0}", data->pMessage);
      return VK_FALSE;
    }
#  endif
  } // namespace

  // ==========================================================================
  // Lifetime
  // ==========================================================================

  VulkanContext::VulkanContext()
  {
    m_viewport = nsRectFloat(0.0f, 0.0f, 1.0f, 1.0f);
  }

  VulkanContext::~VulkanContext()
  {
    Shutdown();
  }

  bool VulkanContext::Initialize(HWND windowHandle, nsUInt32 width, nsUInt32 height)
  {
    if (!windowHandle)
      return false;
    m_window = windowHandle;
    m_width = width;
    m_height = height;

    if (!CreateInstance() || !CreateSurface(windowHandle) || !PickDevice()
      || !CreateDevice())
      return false;
    if (!CreateFrames() || !CreateDescriptorLayouts() || !CreateShaders())
      return false;
    if (!CreateComputePipelines())
      nsLog::Warning(
        "VulkanContext: filter compute shaders unavailable; CSS filters disabled");
    if (!CreateSamplerAndDefaults())
      return false;
    if (!CreateSwapchain() || !CreateDepthBuffer())
      return false;
    if (!SimpleAPHIRenderContext::Initialize(m_device, nsString("Vulkan")))
      return false;

    nsLog::Info("VulkanContext: Vulkan 1.3 renderer initialized ({0}x{1}, {2})", width,
      height, m_deviceProps.deviceName);
    return true;
  }

  void VulkanContext::Shutdown()
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_device == VK_NULL_HANDLE)
    {
      DestroyInstance();
      return;
    }
    vkDeviceWaitIdle(m_device);
    m_shuttingDown = true; // DeferDestroy() now destroys immediately

    // Base first: it owns pooled wrappers whose destructors need a live device.
    SimpleAPHIRenderContext::Shutdown();
    m_atlases.clear();
    m_canvases.clear();
    m_videos.clear();
    m_whiteTexture.reset();
    m_pendingCommandList = HarrlowCommandList();

    for (auto& kv : m_pipelines)
      vkDestroyPipeline(m_device, kv.second, nullptr);
    m_pipelines.clear();
    for (auto& kv : m_computePipelines)
      vkDestroyPipeline(m_device, kv.second, nullptr);
    m_computePipelines.clear();
    if (m_pipelineLayout)
      vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_computePipelineLayout)
      vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
    if (m_uboLayout)
      vkDestroyDescriptorSetLayout(m_device, m_uboLayout, nullptr);
    if (m_textureLayout)
      vkDestroyDescriptorSetLayout(m_device, m_textureLayout, nullptr);
    if (m_computeLayout)
      vkDestroyDescriptorSetLayout(m_device, m_computeLayout, nullptr);
    if (m_vertModule)
      vkDestroyShaderModule(m_device, m_vertModule, nullptr);
    if (m_fragModule)
      vkDestroyShaderModule(m_device, m_fragModule, nullptr);
    if (m_sampler)
      vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_dummyStorage)
      vkDestroyBuffer(m_device, m_dummyStorage, nullptr);
    if (m_dummyStorageMemory)
      vkFreeMemory(m_device, m_dummyStorageMemory, nullptr);
    for (GlyphBuffer& sb : m_glyphBuffers)
    {
      if (sb.mapped)
        vkUnmapMemory(m_device, sb.memory);
      if (sb.buffer)
        vkDestroyBuffer(m_device, sb.buffer, nullptr);
      if (sb.memory)
        vkFreeMemory(m_device, sb.memory, nullptr);
      sb = GlyphBuffer();
    }

    for (Frame& f : m_frames)
    {
      CollectGarbage(f);
      for (RingChunk& c : f.ring)
      {
        if (c.mapped)
          vkUnmapMemory(m_device, c.memory);
        if (c.buffer)
          vkDestroyBuffer(m_device, c.buffer, nullptr);
        if (c.memory)
          vkFreeMemory(m_device, c.memory, nullptr);
      }
      f.ring.clear();
      if (f.pool)
        vkDestroyDescriptorPool(m_device, f.pool, nullptr);
      if (f.fence)
        vkDestroyFence(m_device, f.fence, nullptr);
      if (f.imageAvailable)
        vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
      if (f.renderFinished)
        vkDestroySemaphore(m_device, f.renderFinished, nullptr);
      f = Frame();
    }
    DestroyDepthBuffer();
    DestroySwapchain();
    if (m_commandPool)
      vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
    DestroyInstance();
    m_shuttingDown = false;
  }

  void VulkanContext::DestroyInstance()
  {
    if (m_surface)
      vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
#  if NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    if (m_debugMessenger)
    {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy)
        destroy(m_instance, m_debugMessenger, nullptr);
      m_debugMessenger = VK_NULL_HANDLE;
    }
#  endif
    if (m_instance)
      vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
  }

  // ==========================================================================
  // Setup
  // ==========================================================================

  bool VulkanContext::CreateInstance()
  {
    VkApplicationInfo app = { };
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ApertureUISample";
    app.pEngineName = "ApertureUI";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions
      = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    std::vector<const char*> layers;
#  if NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    // Validation is opt-in (APUI_VK_VALIDATION=1): the layer intercepts every
    // API call and costs ~3 ms of CPU per frame for a 230-command UI, which
    // distorts every development-build frame time. Only request it when the
    // loader has it; asking unconditionally fails vkCreateInstance with
    // VK_ERROR_LAYER_NOT_PRESENT on machines without the SDK layer.
    char validationValue[2] = { };
    const bool wantValidation = GetEnvironmentVariableA("APUI_VK_VALIDATION",
                                  validationValue, sizeof(validationValue))
        == 1
      && validationValue[0] == '1';
    uint32_t layerCount = wantValidation ? 0u : 0u;
    if (wantValidation)
      vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> available(layerCount);
    if (wantValidation)
      vkEnumerateInstanceLayerProperties(&layerCount, available.data());
    for (const VkLayerProperties& lp : available)
      if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0)
        layers.push_back("VK_LAYER_KHRONOS_validation");
    if (wantValidation && layers.empty())
      nsLog::Warning("VulkanContext: VK_LAYER_KHRONOS_validation not installed; running "
                     "without validation");
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#  endif

    VkInstanceCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&info, nullptr, &m_instance);
    if (r != VK_SUCCESS)
    {
      nsLog::Error("VulkanContext: vkCreateInstance failed ({0})", static_cast<int>(r));
      return false;
    }
#  if NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create)
    {
      VkDebugUtilsMessengerCreateInfoEXT dbg = { };
      dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      dbg.pfnUserCallback = DebugCallback;
      create(m_instance, &dbg, nullptr, &m_debugMessenger);
    }
#  endif
    return true;
  }

  bool VulkanContext::CreateSurface(HWND hwnd)
  {
    VkWin32SurfaceCreateInfoKHR info = { };
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandle(nullptr);
    info.hwnd = hwnd;
    return vkCreateWin32SurfaceKHR(m_instance, &info, nullptr, &m_surface) == VK_SUCCESS;
  }

  bool VulkanContext::PickDevice()
  {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    uint32_t bestFamily = 0;
    for (VkPhysicalDevice pd : devices)
    {
      VkPhysicalDeviceProperties props = { };
      vkGetPhysicalDeviceProperties(pd, &props);
      if (props.apiVersion < VK_API_VERSION_1_3)
        continue;
      uint32_t qcount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
      std::vector<VkQueueFamilyProperties> families(qcount);
      vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, families.data());
      for (uint32_t i = 0; i < qcount; ++i)
      {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, m_surface, &present);
        const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & need) == need && present)
        {
          const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
            : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU             ? 1
                                                                                     : 0;
          if (score > bestScore)
          {
            best = pd;
            bestScore = score;
            bestFamily = i;
          }
          break;
        }
      }
    }
    if (best == VK_NULL_HANDLE)
    {
      nsLog::Error("VulkanContext: no Vulkan 1.3 device with graphics+compute+present");
      return false;
    }
    m_physicalDevice = best;
    m_queueFamily = bestFamily;
    vkGetPhysicalDeviceProperties(best, &m_deviceProps);
    m_uboStride = AlignUp(512,
      std::max<VkDeviceSize>(256, m_deviceProps.limits.minUniformBufferOffsetAlignment));
    return true;
  }

  bool VulkanContext::CreateDevice()
  {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = { };
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = m_queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13 = { };
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_FALSE;

    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &f13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device) != VK_SUCCESS)
    {
      nsLog::Error("VulkanContext: vkCreateDevice failed");
      return false;
    }
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo pool = { };
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = m_queueFamily;
    return vkCreateCommandPool(m_device, &pool, nullptr, &m_commandPool) == VK_SUCCESS;
  }

  bool VulkanContext::CreateSwapchain()
  {
    VkSurfaceCapabilitiesKHR caps = { };
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
      m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
      m_physicalDevice, m_surface, &formatCount, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty()
      ? VkSurfaceFormatKHR { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
      : formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
      {
        chosen = f;
        break;
      }
    m_swapchainFormat = chosen.format;

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_vsync)
    {
      uint32_t modeCount = 0;
      vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_physicalDevice, m_surface, &modeCount, nullptr);
      std::vector<VkPresentModeKHR> modes(modeCount);
      vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_physicalDevice, m_surface, &modeCount, modes.data());
      for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR || m == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
          presentMode = m;
          if (m == VK_PRESENT_MODE_MAILBOX_KHR)
            break;
        }
    }

    if (caps.currentExtent.width != UINT32_MAX)
      m_swapchainExtent = caps.currentExtent;
    else
      m_swapchainExtent = { std::clamp(m_width, caps.minImageExtent.width,
                              caps.maxImageExtent.width),
        std::clamp(m_height, caps.minImageExtent.height, caps.maxImageExtent.height) };
    if (m_swapchainExtent.width == 0 || m_swapchainExtent.height == 0)
      return false;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
      imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info = { };
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = m_swapchainExtent;
    info.imageArrayLayers = 1;
    info.imageUsage
      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain) != VK_SUCCESS)
    {
      nsLog::Error("VulkanContext: vkCreateSwapchainKHR failed");
      return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    m_swapchainImages.resize(count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swapchainImages.data());
    m_swapchainViews.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
      VkImageViewCreateInfo view = { };
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = m_swapchainImages[i];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = m_swapchainFormat;
      view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      if (vkCreateImageView(m_device, &view, nullptr, &m_swapchainViews[i]) != VK_SUCCESS)
        return false;
    }
    m_width = m_swapchainExtent.width;
    m_height = m_swapchainExtent.height;
    return true;
  }

  void VulkanContext::DestroySwapchain()
  {
    for (VkImageView v : m_swapchainViews)
      vkDestroyImageView(m_device, v, nullptr);
    m_swapchainViews.clear();
    m_swapchainImages.clear();
    if (m_swapchain)
      vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
  }

  bool VulkanContext::CreateDepthBuffer()
  {
    DestroyDepthBuffer();

    // Clamp the requested MSAA to what colour and depth attachments both support.
    {
      const VkSampleCountFlags supported
        = m_deviceProps.limits.framebufferColorSampleCounts
        & m_deviceProps.limits.framebufferDepthSampleCounts;
      while (m_msaaSamples > VK_SAMPLE_COUNT_1_BIT && !(supported & m_msaaSamples))
        m_msaaSamples = static_cast<VkSampleCountFlagBits>(m_msaaSamples >> 1);
    }
    if (m_msaaSamples > VK_SAMPLE_COUNT_1_BIT)
    {
      VkImageCreateInfo ci = { };
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = m_swapchainFormat;
      ci.extent = { m_swapchainExtent.width, m_swapchainExtent.height, 1 };
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = m_msaaSamples;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(m_device, &ci, nullptr, &m_msaaColorImage) != VK_SUCCESS)
        return false;
      VkMemoryRequirements req = { };
      vkGetImageMemoryRequirements(m_device, m_msaaColorImage, &req);
      VkMemoryAllocateInfo alloc = { };
      alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex
        = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (vkAllocateMemory(m_device, &alloc, nullptr, &m_msaaColorMemory) != VK_SUCCESS)
        return false;
      vkBindImageMemory(m_device, m_msaaColorImage, m_msaaColorMemory, 0);
      VkImageViewCreateInfo vi = { };
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = m_msaaColorImage;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = m_swapchainFormat;
      vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      if (vkCreateImageView(m_device, &vi, nullptr, &m_msaaColorView) != VK_SUCCESS)
        return false;
    }

    const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT };
    m_depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat f : candidates)
    {
      VkFormatProperties props = { };
      vkGetPhysicalDeviceFormatProperties(m_physicalDevice, f, &props);
      if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      {
        m_depthFormat = f;
        break;
      }
    }
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
      return false;

    VkImageCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = m_depthFormat;
    info.extent = { m_swapchainExtent.width, m_swapchainExtent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = m_msaaSamples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &info, nullptr, &m_depthImage) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req = { };
    vkGetImageMemoryRequirements(m_device, m_depthImage, &req);
    VkMemoryAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex
      = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_depthMemory) != VK_SUCCESS)
      return false;
    vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0);

    VkImageViewCreateInfo view = { };
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_depthImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = m_depthFormat;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
      | (m_depthFormat == VK_FORMAT_D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    return vkCreateImageView(m_device, &view, nullptr, &m_depthView) == VK_SUCCESS;
  }

  void VulkanContext::DestroyDepthBuffer()
  {
    vkDestroyImageView(m_device, m_depthView, nullptr);
    vkDestroyImage(m_device, m_depthImage, nullptr);
    vkFreeMemory(m_device, m_depthMemory, nullptr);
    vkDestroyImageView(m_device, m_msaaColorView, nullptr);
    vkDestroyImage(m_device, m_msaaColorImage, nullptr);
    vkFreeMemory(m_device, m_msaaColorMemory, nullptr);
    m_depthView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthMemory = VK_NULL_HANDLE;
    m_msaaColorView = VK_NULL_HANDLE;
    m_msaaColorImage = VK_NULL_HANDLE;
    m_msaaColorMemory = VK_NULL_HANDLE;
    m_depthInitialised = false;
  }

  bool VulkanContext::CreateFrames()
  {
    for (Frame& f : m_frames)
    {
      VkCommandBufferAllocateInfo alloc = { };
      alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      alloc.commandPool = m_commandPool;
      alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc.commandBufferCount = 1;
      if (vkAllocateCommandBuffers(m_device, &alloc, &f.cmd) != VK_SUCCESS)
        return false;

      VkFenceCreateInfo fence = { };
      fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkSemaphoreCreateInfo sem = { };
      sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      if (vkCreateFence(m_device, &fence, nullptr, &f.fence) != VK_SUCCESS
        || vkCreateSemaphore(m_device, &sem, nullptr, &f.imageAvailable) != VK_SUCCESS
        || vkCreateSemaphore(m_device, &sem, nullptr, &f.renderFinished) != VK_SUCCESS)
        return false;

      const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 7 * 16 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * 1024 + 128 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1024 + 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * 1024 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 },
      };
      VkDescriptorPoolCreateInfo pool = { };
      pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pool.maxSets = 1024 + 16 + 64;
      pool.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
      pool.pPoolSizes = sizes;
      if (vkCreateDescriptorPool(m_device, &pool, nullptr, &f.pool) != VK_SUCCESS)
        return false;

      if (!AddRingChunk(f, kRingChunkBytes))
        return false;
    }
    return true;
  }

  bool VulkanContext::CreateDescriptorLayouts()
  {
    // set 0: seven dynamic uniform buffers (bindings 0..6, see kSlotToBinding)
    std::vector<VkDescriptorSetLayoutBinding> ubo;
    for (uint32_t i = 0; i < 7; ++i)
      ubo.push_back({ i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    VkDescriptorSetLayoutCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(ubo.size());
    info.pBindings = ubo.data();
    if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_uboLayout) != VK_SUCCESS)
      return false;

    // set 1: t0..t2 sampled images, s0 sampler (binding 3), glyph buffers t5..t8 (5..8)
    const VkDescriptorSetLayoutBinding tex[] = {
      { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
      { 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
    };
    info.bindingCount = static_cast<uint32_t>(std::size(tex));
    info.pBindings = tex;
    if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_textureLayout)
      != VK_SUCCESS)
      return false;

    const VkDescriptorSetLayout sets[] = { m_uboLayout, m_textureLayout };
    VkPipelineLayoutCreateInfo layout = { };
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 2;
    layout.pSetLayouts = sets;
    if (vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout)
      != VK_SUCCESS)
      return false;

    // compute: b0 ubo, t0/t1 sampled, u0 storage image, s0 sampler (UIFilterCompute.hlsl
    // VK_BIND)
    const VkDescriptorSetLayoutBinding comp[] = {
      { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
      { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
      { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
      { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
      { 4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    info.bindingCount = static_cast<uint32_t>(std::size(comp));
    info.pBindings = comp;
    if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_computeLayout)
      != VK_SUCCESS)
      return false;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_computeLayout;
    return vkCreatePipelineLayout(m_device, &layout, nullptr, &m_computePipelineLayout)
      == VK_SUCCESS;
  }

  VkShaderModule VulkanContext::LoadSpirv(const char* relativePath)
  {
    auto platform = aperture::core::IAPCPlatform::GetEnginePlatform();
    auto* fs = platform ? platform->GetFileSystem() : nullptr;
    if (!fs)
    {
      nsLog::Error("VulkanContext: no file system to load {0}", relativePath);
      return VK_NULL_HANDLE;
    }
    nsDataBuffer data = fs->GetFileData(
      relativePath, aperture::core::IAPCFileSystem::EFileType::OSDependant);
    if (data.IsEmpty() || (data.GetCount() % 4) != 0)
    {
      nsLog::Error(
        "VulkanContext: missing or invalid SPIR-V {0} (build the Vulkan shaders)",
        relativePath);
      return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> words(data.GetCount() / 4);
    std::memcpy(words.data(), data.GetData(), data.GetCount());
    if (words[0] != 0x07230203u)
      return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = data.GetCount();
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(m_device, &info, nullptr, &module);
    return module;
  }

  bool VulkanContext::CreateShaders()
  {
    m_vertModule = LoadSpirv("Shaders/Vulkan/UIPlayer.vert.spv");
    m_fragModule = LoadSpirv("Shaders/Vulkan/UIPlayer.frag.spv");
    return m_vertModule != VK_NULL_HANDLE && m_fragModule != VK_NULL_HANDLE;
  }

  bool VulkanContext::CreateComputePipelines()
  {
    struct Entry
    {
      FilterType type;
      const char* file;
    };
    const Entry entries[] = {
      { FilterType::BlurH, "Shaders/Vulkan/UIFilter.CSBlurH.spv" },
      { FilterType::BlurV, "Shaders/Vulkan/UIFilter.CSBlurV.spv" },
      { FilterType::DropShadow, "Shaders/Vulkan/UIFilter.CSDropShadow.spv" },
      { FilterType::DropShadowCompose,
        "Shaders/Vulkan/UIFilter.CSDropShadowCompose.spv" },
      { FilterType::MaskMultiply, "Shaders/Vulkan/UIFilter.CSMaskMultiply.spv" },
      { FilterType::ColorMatrix, "Shaders/Vulkan/UIFilter.CSColorMatrix.spv" },
    };
    bool all = true;
    for (const Entry& e : entries)
    {
      VkShaderModule module = LoadSpirv(e.file);
      if (module == VK_NULL_HANDLE)
      {
        all = false;
        continue;
      }
      VkComputePipelineCreateInfo info = { };
      info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      info.stage.module = module;
      info.stage.pName = "main"; // slangc names every entry point "main"
      info.layout = m_computePipelineLayout;
      VkPipeline pipeline = VK_NULL_HANDLE;
      if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline)
        == VK_SUCCESS)
        m_computePipelines[static_cast<int>(e.type)] = pipeline;
      else
        all = false;
      vkDestroyShaderModule(m_device, module, nullptr);
    }
    m_supportsCompute = all;
    return all;
  }

  bool VulkanContext::CreateSamplerAndDefaults()
  {
    // Matches the D3D11 sampler: MIN_MAG_LINEAR_MIP_POINT, clamp on all axes.
    VkSamplerCreateInfo sampler = { };
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    if (vkCreateSampler(m_device, &sampler, nullptr, &m_sampler) != VK_SUCCESS)
      return false;

    // 64-byte buffer standing in for the glyph curve/paint/band buffers when the
    // vector-glyph renderer is off (the layout still needs valid descriptors).
    VkBufferCreateInfo buf = { };
    buf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf.size = 64;
    buf.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(m_device, &buf, nullptr, &m_dummyStorage) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req = { };
    vkGetBufferMemoryRequirements(m_device, m_dummyStorage, &req);
    VkMemoryAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex
      = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_dummyStorageMemory) != VK_SUCCESS)
      return false;
    vkBindBufferMemory(m_device, m_dummyStorage, m_dummyStorageMemory, 0);

    const uint8_t white[4] = { 255, 255, 255, 255 };
    VulkanTexture* tex = CreateTextureRaw(1, 1, VK_FORMAT_R8G8B8A8_UNORM, white, 4);
    if (!tex)
      return false;
    m_whiteTexture.reset(tex);
    return true;
  }

  VkPipeline VulkanContext::GetOrCreatePipeline(
    uint32_t blendKey, VkFormat colorFormat, bool hasDepth)
  {
    const uint64_t key = static_cast<uint64_t>(blendKey)
      | (static_cast<uint64_t>(colorFormat) << 32)
      | (static_cast<uint64_t>(hasDepth ? 1 : 0) << 60)
      | (static_cast<uint64_t>(hasDepth ? m_msaaSamples : 1) << 52);
    auto it = m_pipelines.find(key);
    if (it != m_pipelines.end())
      return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = { };
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragModule;
    stages[1].pName = "main";

    // HarrlowVG::Vertex: float2 pos, float2 uv, float4 col (32 bytes)
    VkVertexInputBindingDescription binding
      = { 0, sizeof(HarrlowVG::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    const VkVertexInputAttributeDescription attrs[3] = {
      { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
      { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 },
      { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = { };
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly = { };
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport = { };
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = { };
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = { };
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    // Only the swapchain pass (the one with depth) is multisampled; layer RTs are 1x.
    multisample.rasterizationSamples = hasDepth ? m_msaaSamples : VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth = { };
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState attach = { };
    attach.blendEnable = (blendKey & 1u) ? VK_TRUE : VK_FALSE;
    attach.srcColorBlendFactor = static_cast<VkBlendFactor>((blendKey >> 8) & 0xFF);
    attach.dstColorBlendFactor = static_cast<VkBlendFactor>((blendKey >> 16) & 0xFF);
    attach.colorBlendOp = static_cast<VkBlendOp>((blendKey >> 24) & 0xFF);
    attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attach.alphaBlendOp = VK_BLEND_OP_ADD;
    attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = { };
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attach;

    const VkDynamicState dynamics[2]
      = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { };
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    const bool depthHasStencil = hasDepth && m_depthFormat != VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering = { };
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = hasDepth ? m_depthFormat : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat
      = depthHasStencil ? m_depthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info = { };
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
    info.layout = m_pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline)
      != VK_SUCCESS)
    {
      nsLog::Error(
        "VulkanContext: graphics pipeline creation failed (blend {0})", blendKey);
      return VK_NULL_HANDLE;
    }
    m_pipelines[key] = pipeline;
    return pipeline;
  }

  uint32_t VulkanContext::FindMemoryTypeIndex(
    uint32_t typeFilter, VkMemoryPropertyFlags props) const
  {
    VkPhysicalDeviceMemoryProperties mem = { };
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
      if ((typeFilter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
        return i;
    return 0;
  }

  void VulkanContext::SetMSAASampleCount(nsGALMSAASampleCount::Enum count)
  {
    VkSampleCountFlagBits wanted = VK_SAMPLE_COUNT_1_BIT;
    switch (static_cast<int>(count))
    {
      case 2:
        wanted = VK_SAMPLE_COUNT_2_BIT;
        break;
      case 4:
        wanted = VK_SAMPLE_COUNT_4_BIT;
        break;
      case 8:
        wanted = VK_SAMPLE_COUNT_8_BIT;
        break;
      default:
        break;
    }
    if (wanted == m_msaaSamples)
      return;
    m_msaaSamples = wanted;
    m_swapchainDirty
      = true; // rebuilt (and clamped to device limits) on the next BeginFrame
  }


  // ==========================================================================
  // Frame ring / garbage
  // ==========================================================================

  bool VulkanContext::AddRingChunk(Frame& frame, VkDeviceSize minCapacity)
  {
    RingChunk chunk;
    chunk.capacity = std::max(minCapacity, kRingChunkBytes);
    VkBufferCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = chunk.capacity;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
      | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &chunk.buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req = { };
    vkGetBufferMemoryRequirements(m_device, chunk.buffer, &req);
    VkMemoryAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &chunk.memory) != VK_SUCCESS)
    {
      vkDestroyBuffer(m_device, chunk.buffer, nullptr);
      return false;
    }
    // (mapped below; the first touch of every page happens here, not in a draw)
    vkBindBufferMemory(m_device, chunk.buffer, chunk.memory, 0);
    if (vkMapMemory(m_device, chunk.memory, 0, chunk.capacity, 0, &chunk.mapped)
      != VK_SUCCESS)
    {
      vkDestroyBuffer(m_device, chunk.buffer, nullptr);
      vkFreeMemory(m_device, chunk.memory, nullptr);
      return false;
    }
    if (chunk.mapped)
      std::memset(chunk.mapped, 0, static_cast<size_t>(chunk.capacity));
    frame.ring.push_back(chunk);
    return true;
  }

  VulkanContext::RingSlice VulkanContext::RingAlloc(VkDeviceSize size, VkDeviceSize align)
  {
    Frame& f = CurrentFrame();
    // Keep 512 bytes of slack so a descriptor range of one UBO slot never
    // reaches past the end of the chunk.
    for (;;)
    {
      RingChunk& c = f.ring[f.ringChunk];
      const VkDeviceSize head = AlignUp(c.head, align);
      if (head + size + 1024 <= c.capacity)
      {
        RingSlice s;
        s.buffer = c.buffer;
        s.offset = head;
        s.mapped = static_cast<uint8_t*>(c.mapped) + head;
        s.chunkIndex = static_cast<uint32_t>(f.ringChunk);
        c.head = head + size;
        return s;
      }
      if (f.ringChunk + 1 >= f.ring.size())
      {
        if (!AddRingChunk(f, std::max(c.capacity * 2, size + 1024 * 1024)))
        {
          nsLog::Error("VulkanContext: out of ring memory ({0} bytes requested)", size);
          return RingSlice();
        }
      }
      ++f.ringChunk;
      f.ring[f.ringChunk].head = 0;
    }
  }

  VkDescriptorSet VulkanContext::GetUboSet(RingChunk& chunk)
  {
    if (chunk.uboSet != VK_NULL_HANDLE)
      return chunk.uboSet;
    Frame& f = CurrentFrame();
    VkDescriptorSetAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = f.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_uboLayout;
    if (vkAllocateDescriptorSets(m_device, &alloc, &chunk.uboSet) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    VkDescriptorBufferInfo infos[7];
    VkWriteDescriptorSet writes[7] = { };
    for (uint32_t i = 0; i < 7; ++i)
    {
      infos[i] = { chunk.buffer, 0, m_uboStride };
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = chunk.uboSet;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 7, writes, 0, nullptr);
    return chunk.uboSet;
  }

  void VulkanContext::DeferDestroy(std::function<void()> fn)
  {
    if (m_shuttingDown || m_device == VK_NULL_HANDLE)
    {
      fn();
      return;
    }
    // A slot's garbage is freed once that slot's fence has been waited (see
    // EnsureFrameOpen). While the current slot is open, anything released now
    // was last used by the commands being recorded, so its own fence is the
    // right one. Once EndFrame advanced m_frameIndex, the current slot is the
    // NEXT one to open, and its fence belongs to the frame before the one that
    // was just submitted: a texture released in that gap (a pool evicting a
    // render target before the next frame is lazily opened) would be freed
    // while the submitted frame still samples it, and the GPU faults. The
    // frame that was just submitted owns the previous slot.
    //
    // Objects are released on whatever thread drops the last reference (a UI
    // thread evicts render targets while the game thread records), so this does
    // not take the frame lock. m_garbageMutex is a leaf lock and also guards the
    // state the choice depends on: EndFrame clears Frame::open and advances
    // m_frameIndex, and EnsureFrameOpen sets Frame::open, under it, so the slot
    // is picked from one consistent view and never from the half updated state
    // between those two writes.
    std::lock_guard<std::mutex> lock(m_garbageMutex);
    Frame& current = m_frames[m_frameIndex];
    const uint32_t previous
      = (m_frameIndex + kMaxFramesInFlight - 1) % kMaxFramesInFlight;
    Frame& owner = current.open ? current : m_frames[previous];
    owner.garbage.push_back(std::move(fn));
  }

  void VulkanContext::CollectGarbage(Frame& frame)
  {
    // Run the callbacks outside the mutex: they may release further objects.
    // The list is swapped with a vector that kept its capacity from the last
    // call, and cleared (not freed) afterwards, so a frame with garbage does not
    // allocate. Taking the scratch vector out of the member first keeps a nested
    // call from a callback safe: it finds the member empty and starts its own.
    // Callers hold m_apiMutex, which is what guards m_garbageScratch.
    std::vector<std::function<void()>> doomed;
    doomed.swap(m_garbageScratch);
    {
      std::lock_guard<std::mutex> lock(m_garbageMutex);
      doomed.swap(frame.garbage);
    }
    for (auto& fn : doomed)
      fn();
    doomed.clear();
    m_garbageScratch.swap(doomed);
  }

  void VulkanContext::EnsureFrameOpen()
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_device == VK_NULL_HANDLE)
      return;
    Frame& f = CurrentFrame();
    if (f.open)
      return;
    if (f.submittedFence)
    {
      vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
      vkResetFences(m_device, 1, &f.fence);
      f.submittedFence = false;
    }
    CollectGarbage(f);
    vkResetCommandBuffer(f.cmd, 0);
    vkResetDescriptorPool(m_device, f.pool, 0);
    f.textureSets.clear();
    for (RingChunk& c : f.ring)
    {
      c.head = 0;
      c.uboSet = VK_NULL_HANDLE;
    }
    f.ringChunk = 0;

    VkCommandBufferBeginInfo begin = { };
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &begin);
    {
      std::lock_guard<std::mutex> lock(m_garbageMutex); // see DeferDestroy
      f.open = true;
    }
    f.acquired = false;
    m_renderingActive = false;
    m_mainTargetActive = false;
    m_layerTarget = nullptr;
    m_boundPipeline = VK_NULL_HANDLE;
    m_viewportDirty = true;
    m_vertexSlice = RingSlice();
    m_indexSlice = RingSlice();
  }

  // ==========================================================================
  // Rendering state
  // ==========================================================================

  void VulkanContext::FullBarrier(VkCommandBuffer cmd)
  {
    VkMemoryBarrier barrier = { };
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }

  void VulkanContext::BeginRenderingTo(VulkanTexture* target, bool clear)
  {
    VkCommandBuffer cmd = CurrentFrame().cmd;
    if (!target->inGeneralLayout)
    {
      VkImageMemoryBarrier b = { };
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = target->image();
      b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      target->inGeneralLayout = true;
    }
    FullBarrier(cmd);

    VkRenderingAttachmentInfo color = { };
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target->view();
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

    VkRenderingInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = { { 0, 0 }, { target->GetWidth(), target->GetHeight() } };
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &info);

    m_renderingActive = true;
    m_targetExtent = { target->GetWidth(), target->GetHeight() };
    m_targetFormat = target->format();
    m_boundPipeline = VK_NULL_HANDLE;
    m_viewportDirty = true;
  }

  void VulkanContext::BeginMainRendering(bool clear)
  {
    VkCommandBuffer cmd = CurrentFrame().cmd;
    VkRenderingAttachmentInfo color = { };
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_swapchainViews[m_imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color
      = { { m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a } };
    if (m_msaaColorView != VK_NULL_HANDLE)
    {
      // Render multisampled, resolve into the swapchain image at every end of
      // rendering (suspensions resolve too; the MSAA image is kept for LOAD).
      color.imageView = m_msaaColorView;
      color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView = m_swapchainViews[m_imageIndex];
      color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingAttachmentInfo depth = { };
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = { { 0, 0 }, m_swapchainExtent };
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    info.pDepthAttachment = &depth;
    info.pStencilAttachment = m_depthFormat == VK_FORMAT_D32_SFLOAT ? nullptr : &depth;
    if (!clear)
      FullBarrier(cmd);
    vkCmdBeginRendering(cmd, &info);

    m_renderingActive = true;
    m_targetExtent = m_swapchainExtent;
    m_targetFormat = m_swapchainFormat;
    m_boundPipeline = VK_NULL_HANDLE;
    m_viewportDirty = true;
  }

  void VulkanContext::EndRendering()
  {
    if (!m_renderingActive)
      return;
    VkCommandBuffer cmd = CurrentFrame().cmd;
    vkCmdEndRendering(cmd);
    m_renderingActive = false;
    FullBarrier(cmd);
  }

  void VulkanContext::EnsureRenderingActive()
  {
    if (m_renderingActive)
      return;
    if (m_layerTarget)
      BeginRenderingTo(m_layerTarget, false);
    else if (m_mainTargetActive)
      BeginMainRendering(false);
  }

  void VulkanContext::ApplyViewportAndScissor()
  {
    VkCommandBuffer cmd = CurrentFrame().cmd;
    const float w = m_viewport.width > 0.0f ? m_viewport.width
                                            : static_cast<float>(m_targetExtent.width);
    const float h = m_viewport.height > 0.0f ? m_viewport.height
                                             : static_cast<float>(m_targetExtent.height);
    // Negative height flips Y so the D3D-convention HLSL (NDC +Y up) lands right way up.
    VkViewport vp = { };
    vp.x = m_viewport.x + m_screenOffsetX;
    vp.y = m_viewport.y + m_screenOffsetY + h;
    vp.width = w;
    vp.height = -h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc = m_scissor;
    const int32_t maxX = static_cast<int32_t>(m_targetExtent.width);
    const int32_t maxY = static_cast<int32_t>(m_targetExtent.height);
    sc.offset.x = std::clamp(
      sc.offset.x + static_cast<int32_t>(m_screenOffsetX), 0, std::max(0, maxX - 1));
    sc.offset.y = std::clamp(
      sc.offset.y + static_cast<int32_t>(m_screenOffsetY), 0, std::max(0, maxY - 1));
    sc.extent.width = static_cast<uint32_t>(std::clamp<int64_t>(
      static_cast<int64_t>(m_scissor.extent.width), 1, maxX - sc.offset.x));
    sc.extent.height = static_cast<uint32_t>(std::clamp<int64_t>(
      static_cast<int64_t>(m_scissor.extent.height), 1, maxY - sc.offset.y));
    vkCmdSetScissor(cmd, 0, 1, &sc);
  }

  VkDescriptorSet VulkanContext::GetTextureSet(
    VulkanTexture* t0, VulkanTexture* t1, VulkanTexture* t2)
  {
    Frame& f = CurrentFrame();
    VulkanTexture* white = m_whiteTexture.get();
    if (!t0)
      t0 = white;
    if (!t1)
      t1 = white;
    if (!t2)
      t2 = white;
    uint64_t key = reinterpret_cast<uint64_t>(t0->view());
    key = key * 1099511628211ull ^ reinterpret_cast<uint64_t>(t1->view());
    key = key * 1099511628211ull ^ reinterpret_cast<uint64_t>(t2->view());
    for (const GlyphBuffer& sb : m_glyphBuffers)
      key = key * 1099511628211ull ^ reinterpret_cast<uint64_t>(sb.buffer);
    auto it = f.textureSets.find(key);
    if (it != f.textureSets.end())
      return it->second;

    VkDescriptorSetAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = f.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_textureLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &alloc, &set) != VK_SUCCESS)
    {
      static bool s_warned = false;
      if (!s_warned)
      {
        s_warned = true;
        nsLog::Error("VulkanContext: texture descriptor pool exhausted this frame");
      }
      return VK_NULL_HANDLE;
    }
    const VkDescriptorImageInfo images[3] = {
      { VK_NULL_HANDLE, t0->view(), VK_IMAGE_LAYOUT_GENERAL },
      { VK_NULL_HANDLE, t1->view(), VK_IMAGE_LAYOUT_GENERAL },
      { VK_NULL_HANDLE, t2->view(), VK_IMAGE_LAYOUT_GENERAL },
    };
    const VkDescriptorImageInfo samplerInfo
      = { m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkDescriptorBufferInfo storage[4];
    for (uint32_t i = 0; i < 4; ++i)
      storage[i] = { m_glyphBuffers[i].buffer ? m_glyphBuffers[i].buffer : m_dummyStorage,
        0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[8] = { };
    for (uint32_t i = 0; i < 3; ++i)
    {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[i].pImageInfo = &images[i];
    }
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[3].pImageInfo = &samplerInfo;
    for (uint32_t i = 4; i < 8; ++i)
    {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i + 1; // 5..8
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &storage[i - 4];
    }
    vkUpdateDescriptorSets(m_device, 8, writes, 0, nullptr);
    f.textureSets[key] = set;
    return set;
  }

  void VulkanContext::FlushDrawState()
  {
    EnsureRenderingActive();
    if (!m_renderingActive)
      return;
    Frame& f = CurrentFrame();
    VkCommandBuffer cmd = f.cmd;

    VkPipeline pipeline = GetOrCreatePipeline(
      m_blendKey, m_targetFormat, m_mainTargetActive && !m_layerTarget);
    if (pipeline == VK_NULL_HANDLE)
      return;
    if (pipeline != m_boundPipeline)
    {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      m_boundPipeline = pipeline;
    }
    if (m_viewportDirty)
    {
      ApplyViewportAndScissor();
      m_viewportDirty = false;
    }

    // One contiguous block of 7 constant slots per draw: the engine rewrites
    // its per-slot buffers between batches, so every draw snapshots them.
    RingSlice block = RingAlloc(7 * m_uboStride, 256);
    if (!block.buffer)
      return;
    uint8_t* base = static_cast<uint8_t*>(block.mapped);
    std::memset(base, 0, static_cast<size_t>(7 * m_uboStride));
    WriteViewportConstants();
    uint32_t dynamicOffsets[7];
    for (uint32_t i = 0; i < 7; ++i)
    {
      const auto& src = m_uboSources[i];
      if (src.first && src.second)
        std::memcpy(base + i * m_uboStride, src.first,
          std::min<size_t>(src.second, static_cast<size_t>(m_uboStride)));
      dynamicOffsets[i] = static_cast<uint32_t>(block.offset + i * m_uboStride);
    }
    VkDescriptorSet set0 = GetUboSet(f.ring[block.chunkIndex]);
    VkDescriptorSet set1
      = GetTextureSet(m_boundTextures[0], m_boundTextures[1], m_boundTextures[2]);
    if (set0 == VK_NULL_HANDLE || set1 == VK_NULL_HANDLE)
      return;
    const VkDescriptorSet sets[2] = { set0, set1 };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2,
      sets, 7, dynamicOffsets);
  }

  void VulkanContext::WriteViewportConstants()
  {
    // Binding 0 = ViewportCB: origin.xy, size.xy, colorMgmt.xyzw
    float* v = m_viewportConstants;
    v[0] = m_viewport.x;
    v[1] = m_viewport.y;
    v[2] = m_viewport.width <= 0.0f ? 1.0f : m_viewport.width;
    v[3] = m_viewport.height <= 0.0f ? 1.0f : m_viewport.height;
    v[4] = v[5] = v[6] = v[7] = 0.0f;
    m_uboSources[0] = { v, 8 * sizeof(float) };
  }

  // ==========================================================================
  // IPlayerRenderContext frame API
  // ==========================================================================

  void VulkanContext::BeginFrame(const nsColor& clearColor)
  {
    // Held until EndFrame: a host that builds UI command lists on another
    // thread must not open, reset or garbage-collect this frame while it is
    // being recorded. Its build blocks here instead.
    if (!m_frameLock.owns_lock())
      m_frameLock = std::unique_lock<std::recursive_mutex>(m_apiMutex);
    auto releaseOnEarlyReturn = [this]()
    {
      if (m_frameLock.owns_lock())
        m_frameLock.unlock();
    };
    if (m_device == VK_NULL_HANDLE)
    {
      releaseOnEarlyReturn();
      return;
    }
    EnsureFrameOpen();
    Frame& f = CurrentFrame();
    m_clearColor = clearColor;

    if (m_swapchainDirty || m_width != m_swapchainExtent.width
      || m_height != m_swapchainExtent.height)
    {
      vkDeviceWaitIdle(m_device);
      DestroySwapchain();
      m_swapchainDirty = false;
      if (!CreateSwapchain() || !CreateDepthBuffer())
      {
        m_swapchainDirty = true; // minimised window: try again next frame
        releaseOnEarlyReturn();
        return;
      }
    }

    VkResult r = vkAcquireNextImageKHR(
      m_device, m_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_swapchainDirty = true;
      releaseOnEarlyReturn();
      return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
      nsLog::Error(
        "VulkanContext: vkAcquireNextImageKHR failed ({0})", static_cast<int>(r));
      releaseOnEarlyReturn();
      return;
    }
    f.acquired = true;

    EndRendering();
    VkImageMemoryBarrier barriers[3] = { };
    uint32_t count = 1;
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = barriers[0].dstQueueFamilyIndex
      = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = m_swapchainImages[m_imageIndex];
    barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (!m_depthInitialised)
    {
      barriers[1] = barriers[0];
      barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      barriers[1].image = m_depthImage;
      barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
        | (m_depthFormat == VK_FORMAT_D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
      barriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      count = 2;
      m_depthInitialised = true;
    }
    if (m_msaaColorImage != VK_NULL_HANDLE)
    {
      barriers[count] = barriers[0];
      barriers[count].image = m_msaaColorImage;
      ++count;
    }
    vkCmdPipelineBarrier(f.cmd,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
      0, 0, nullptr, 0, nullptr, count, barriers);

    if (m_preSceneCallback)
    {
      const SceneFrame frame { f.cmd, m_swapchainFormat, m_depthFormat, m_swapchainExtent,
        m_msaaSamples, m_frameIndex };
      m_preSceneCallback(frame);
      m_boundPipeline = VK_NULL_HANDLE;
    }

    m_mainTargetActive = true;
    m_layerTarget = nullptr;
    m_viewport = nsRectFloat(0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width),
      static_cast<float>(m_swapchainExtent.height));
    m_scissor = { { 0, 0 }, m_swapchainExtent };
    BeginMainRendering(true);
    ApplyViewportAndScissor();
    m_viewportDirty = false;

    if (m_sceneCallback)
    {
      const SceneFrame frame { f.cmd, m_swapchainFormat, m_depthFormat, m_swapchainExtent,
        m_msaaSamples, m_frameIndex };
      m_sceneCallback(frame);
      m_boundPipeline = VK_NULL_HANDLE;
      m_viewportDirty = true;
    }
  }

  void VulkanContext::RenderCommands(const HarrlowCommandList& list)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_pendingCommandList = list;
  }

  void VulkanContext::SubmitCommands()
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_pendingCommandList.commands.empty())
      return;
    SubmitHarrlowCommands(m_pendingCommandList);
    m_pendingCommandList.commands.clear();
  }

  void VulkanContext::SubmitHarrlowCommands(const HarrlowCommandList& commandList)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_device == VK_NULL_HANDLE)
      return;
    EnsureFrameOpen();
    if (m_frameCounter % 120 == 0)
      EvictStaleSurfaces();
    UploadGlyphBuffers();
    SimpleAPHIRenderContext::SubmitHarrlowCommands(commandList);
  }

  bool VulkanContext::UploadGlyphBuffer(
    GlyphBuffer& sb, const void* data, VkDeviceSize bytes, bool dirty)
  {
    if (!data || bytes == 0)
      return sb.buffer != VK_NULL_HANDLE;
    if (!dirty && sb.buffer && bytes <= sb.capacity)
      return true;
    if (sb.buffer && sb.mapped && bytes <= sb.capacity)
    {
      // Append-only data: in-flight frames only read ranges that still hold
      // their old contents, so refresh in place instead of reallocating.
      std::memcpy(sb.mapped, data, static_cast<size_t>(bytes));
      return true;
    }
    if (sb.buffer)
    {
      // In-flight frames may still read the old buffer: free it with the frame.
      VkBuffer oldBuf = sb.buffer;
      VkDeviceMemory oldMem = sb.memory;
      DeferDestroy(
        [this, oldBuf, oldMem]()
        {
          vkUnmapMemory(m_device, oldMem);
          vkDestroyBuffer(m_device, oldBuf, nullptr);
          vkFreeMemory(m_device, oldMem, nullptr);
        });
      sb = GlyphBuffer();
    }
    const VkDeviceSize capacity = std::max<VkDeviceSize>(bytes * 2, 64 * 1024);
    VkBufferCreateInfo info = { };
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &sb.buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req = { };
    vkGetBufferMemoryRequirements(m_device, sb.buffer, &req);
    VkMemoryAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &sb.memory) != VK_SUCCESS)
    {
      vkDestroyBuffer(m_device, sb.buffer, nullptr);
      sb = GlyphBuffer();
      return false;
    }
    vkBindBufferMemory(m_device, sb.buffer, sb.memory, 0);
    if (vkMapMemory(m_device, sb.memory, 0, capacity, 0, &sb.mapped) == VK_SUCCESS)
      std::memcpy(sb.mapped, data, static_cast<size_t>(bytes));
    sb.capacity = capacity;
    return true;
  }

  void VulkanContext::UploadGlyphBuffers()
  {
#  if NS_ENABLED(APUI_SDK_CONFIG_VECTOR_GLYPH_RENDERING)
    if (!aperture::html::FontEngineBridge::IsVectorGlyphRenderingEnabled())
      return;
    auto* fontEngine = aperture::html::FontEngineBridge::Instance().GetFontEngine();
    if (!fontEngine)
      return;
    // Mirrors D3D11Context: curves (t5) + bands (t7/t8) re-upload when
    // the curve buffer is dirty, paint commands (t6) when the encoder is.
    if (auto* curves = fontEngine->GetVectorGlyphCurveBuffer())
    {
      if (curves->GetSegmentCount() > 0)
      {
        const bool dirty = curves->IsDirty();
        UploadGlyphBuffer(
          m_glyphBuffers[0], curves->GetData(), curves->GetSizeInBytes(), dirty);
        UploadGlyphBuffer(m_glyphBuffers[2], curves->GetBandSetData(),
          curves->GetBandSetSizeInBytes(), dirty);
        UploadGlyphBuffer(m_glyphBuffers[3], curves->GetBandIndexData(),
          curves->GetBandIndexSizeInBytes(), dirty);
        curves->ResetDirty();
      }
    }
    if (auto* paint = fontEngine->GetVectorGlyphPaintEncoder())
    {
      if (paint->GetCommandCount() > 0)
      {
        UploadGlyphBuffer(m_glyphBuffers[1], paint->GetCommandData(),
          paint->GetCommandSizeInBytes(), paint->IsCommandBufferDirty());
        paint->ResetCommandBufferDirty();
      }
    }
#  endif
  }

  namespace
  {
    /// Releases the frame-long API lock however EndFrame returns.
    struct FrameLockRelease
    {
      std::unique_lock<std::recursive_mutex>& lock;
      ~FrameLockRelease()
      {
        if (lock.owns_lock())
          lock.unlock();
      }
    };
  } // namespace

  void VulkanContext::EndFrame(bool enableVSync)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    FrameLockRelease _release { m_frameLock };
    if (m_device == VK_NULL_HANDLE)
      return;
    if (enableVSync != m_vsync)
    {
      m_vsync = enableVSync;
      m_swapchainDirty = true;
    }
    Frame& f = CurrentFrame();
    if (!f.open)
      return;

    EndRendering();
    VkBuffer captureBuffer = VK_NULL_HANDLE;
    VkDeviceMemory captureMemory = VK_NULL_HANDLE;
    if (f.acquired)
    {
      VkImage image = m_swapchainImages[m_imageIndex];
      VkImageMemoryBarrier b = { };
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = image;
      b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      if (!m_capturePath.empty())
      {
        // Live-frame readback for RequestBackbufferCapture: copy the presented image to a
        // host buffer inside this submission, read it after the fence.
        VkBufferCreateInfo info = { };
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = static_cast<VkDeviceSize>(m_swapchainExtent.width)
          * m_swapchainExtent.height * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &info, nullptr, &captureBuffer) == VK_SUCCESS)
        {
          VkMemoryRequirements req = { };
          vkGetBufferMemoryRequirements(m_device, captureBuffer, &req);
          VkMemoryAllocateInfo alloc = { };
          alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          alloc.allocationSize = req.size;
          alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
          if (vkAllocateMemory(m_device, &alloc, nullptr, &captureMemory) == VK_SUCCESS)
          {
            vkBindBufferMemory(m_device, captureBuffer, captureMemory, 0);
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy region = { };
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { m_swapchainExtent.width, m_swapchainExtent.height, 1 };
            vkCmdCopyImageToBuffer(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              captureBuffer, 1, &region);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          }
          else
          {
            vkDestroyBuffer(m_device, captureBuffer, nullptr);
            captureBuffer = VK_NULL_HANDLE;
          }
        }
      }
      if (b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
      {
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      }
      b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      b.dstAccessMask = 0;
      vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(f.cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = { };
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f.cmd;
    if (f.acquired)
    {
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &f.imageAvailable;
      submit.pWaitDstStageMask = &waitStage;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &f.renderFinished;
    }
    if (vkQueueSubmit(m_queue, 1, &submit, f.fence) != VK_SUCCESS)
      nsLog::Error("VulkanContext: vkQueueSubmit failed");
    f.submittedFence = true;

    if (f.acquired)
    {
      VkPresentInfoKHR present = { };
      present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      present.waitSemaphoreCount = 1;
      present.pWaitSemaphores = &f.renderFinished;
      present.swapchainCount = 1;
      present.pSwapchains = &m_swapchain;
      present.pImageIndices = &m_imageIndex;
      VkResult r = vkQueuePresentKHR(m_queue, &present);
      if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        m_swapchainDirty = true;
    }

    if (captureBuffer != VK_NULL_HANDLE)
    {
      vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
      void* mapped = nullptr;
      if (vkMapMemory(m_device, captureMemory, 0, VK_WHOLE_SIZE, 0, &mapped)
        == VK_SUCCESS)
      {
        const uint32_t w = m_swapchainExtent.width, h = m_swapchainExtent.height;
        if (FILE* fp = std::fopen(m_capturePath.c_str(), "wb"))
        {
          const uint32_t rowBytes = (w * 3 + 3) & ~3u;
          const uint32_t dataBytes = rowBytes * h;
          uint8_t header[54] = { 'B', 'M' };
          auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
          auto put16 = [&](int at, uint16_t v) { std::memcpy(header + at, &v, 2); };
          put32(2, 54 + dataBytes);
          put32(10, 54);
          put32(14, 40);
          put32(18, w);
          put32(22, h);
          put16(26, 1);
          put16(28, 24);
          put32(34, dataBytes);
          std::fwrite(header, 1, 54, fp);
          std::vector<uint8_t> row(rowBytes, 0);
          const bool bgra = m_swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM;
          for (int y = static_cast<int>(h) - 1; y >= 0; --y)
          {
            const uint8_t* src
              = static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * w * 4;
            for (uint32_t x = 0; x < w; ++x)
            {
              row[x * 3 + 0] = bgra ? src[x * 4 + 0] : src[x * 4 + 2];
              row[x * 3 + 1] = src[x * 4 + 1];
              row[x * 3 + 2] = bgra ? src[x * 4 + 2] : src[x * 4 + 0];
            }
            std::fwrite(row.data(), 1, rowBytes, fp);
          }
          std::fclose(fp);
          nsLog::Info("VulkanContext: captured backbuffer to {0}", m_capturePath.c_str());
        }
        vkUnmapMemory(m_device, captureMemory);
      }
      vkDestroyBuffer(m_device, captureBuffer, nullptr);
      vkFreeMemory(m_device, captureMemory, nullptr);
      m_capturePath.clear();
    }

    f.acquired = false;
    m_mainTargetActive = false;
    m_layerTarget = nullptr;
    m_renderingActive = false;
    {
      // Closing the slot and advancing the index must look like one step to
      // DeferDestroy (it runs on other threads without the frame lock).
      std::lock_guard<std::mutex> lock(m_garbageMutex);
      f.open = false;
      ++m_frameCounter;
      m_frameIndex = (m_frameIndex + 1) % kMaxFramesInFlight;
    }
  }

  void VulkanContext::Resize(nsUInt32 width, nsUInt32 height)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (width == 0 || height == 0)
      return;
    m_width = width;
    m_height = height;
    m_swapchainDirty = true;
  }

  void VulkanContext::SetViewportScreenOffset(float x, float y)
  {
    m_screenOffsetX = x;
    m_screenOffsetY = y;
    m_viewportDirty = true;
  }

  void VulkanContext::WaitForGPU()
  {
    if (m_device)
      vkDeviceWaitIdle(m_device);
  }

  // ==========================================================================
  // Buffers
  // ==========================================================================

  nsObjectPtr<APHIBuffer> VulkanContext::CreateVertexBuffer(
    const void* data, size_t size, size_t stride, const nsVariantDictionary&)
  {
    if (size == 0)
      return { };
    auto* buf = new VulkanBuffer(size, stride == sizeof(float));
    nsObjectPtr<APHIBuffer> owner(buf);
    if (data)
      UpdateBuffer(buf, data, size, 0);
    return owner;
  }

  nsObjectPtr<APHIBuffer> VulkanContext::CreateIndexBuffer(
    const void* data, size_t size, const nsString&, const nsVariantDictionary&)
  {
    if (size == 0)
      return { };
    auto* buf = new VulkanBuffer(size, false);
    nsObjectPtr<APHIBuffer> owner(buf);
    if (data)
      UpdateBuffer(buf, data, size, 0);
    return owner;
  }

  void VulkanContext::UpdateBuffer(
    APHIBuffer* buffer, const void* data, size_t size, size_t)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!buffer || !data || size == 0)
      return;
    auto* buf = static_cast<VulkanBuffer*>(buffer);
    if (buf->IsConstant())
    {
      if (buf->cpu.size() < size)
        buf->cpu.resize(size);
      std::memcpy(buf->cpu.data(), data, size);
      return;
    }
    if (!aperture::ApertureSDK::IsGeometryUploadCacheEnabled())
    {
      // No geometry cache: nothing re-stages from buf->cpu later this frame,
      // so skip the intermediate copy and write the ring slice directly.
      buf->cpu.clear();
      EnsureFrameOpen();
      RingSlice slice = RingAlloc(size, 16);
      if (!slice.buffer)
        return;
      std::memcpy(slice.mapped, data, size);
      buf->slice = slice;
      buf->frame = m_frameCounter;
      return;
    }
    buf->cpu.assign(
      static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    StageBuffer(buf);
  }

  void VulkanContext::StageBuffer(VulkanBuffer* buf)
  {
    if (buf->cpu.empty())
      return;
    EnsureFrameOpen();
    RingSlice slice = RingAlloc(buf->cpu.size(), 16);
    if (!slice.buffer)
      return;
    std::memcpy(slice.mapped, buf->cpu.data(), buf->cpu.size());
    buf->slice = slice;
    buf->frame = m_frameCounter;
  }

  void VulkanContext::SetVertexBuffer(nsUInt32, APHIBuffer* buffer, size_t offset, size_t)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    auto* buf = static_cast<VulkanBuffer*>(buffer);
    if (buf && buf->frame != m_frameCounter)
      StageBuffer(buf); // geometry-cache hit from an earlier frame: ring slice is gone
    m_vertexSlice = buf ? buf->slice : RingSlice();
    m_vertexOffset = offset;
    m_vertexFrame = buf ? buf->frame : 0;
  }

  void VulkanContext::SetIndexBuffer(APHIBuffer* buffer, size_t offset)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    auto* buf = static_cast<VulkanBuffer*>(buffer);
    if (buf && buf->frame != m_frameCounter)
      StageBuffer(buf);
    m_indexSlice = buf ? buf->slice : RingSlice();
    m_indexOffset = offset;
    m_indexFrame = buf ? buf->frame : 0;
  }

  void VulkanContext::SetConstantBuffer(
    nsUInt32 slot, APHIBuffer* buffer, size_t, size_t size)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (slot >= 7 || !buffer)
      return;
    auto* buf = static_cast<VulkanBuffer*>(buffer);
    if (!buf->IsConstant())
      return;
    m_uboSources[kSlotToBinding[slot]]
      = { buf->cpu.data(), std::min(size ? size : buf->cpu.size(), buf->cpu.size()) };
  }

  void VulkanContext::SetTexture(nsUInt32 slot, APHITexture* texture, APHISampler*)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (slot < 3)
      m_boundTextures[slot] = static_cast<VulkanTexture*>(texture);
  }

  void VulkanContext::SetViewport(const nsRectFloat& rect, float, float)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_viewport = rect;
    m_viewportDirty = true;
  }

  void VulkanContext::SetScissorRect(const nsRectU32& rect)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_scissor.offset = { static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y) };
    m_scissor.extent = { rect.width, rect.height };
    m_viewportDirty = true;
  }

  void VulkanContext::SetBlendState(bool enableBlend, const nsString& srcBlend,
    const nsString& dstBlend, const nsString& blendOp)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_blendKey = (enableBlend ? 1u : 0u)
      | (static_cast<uint32_t>(BlendFactorFromString(srcBlend)) << 8)
      | (static_cast<uint32_t>(BlendFactorFromString(dstBlend)) << 16)
      | (static_cast<uint32_t>(BlendOpFromString(blendOp)) << 24);
  }

  void VulkanContext::Draw(
    nsUInt32 vertexCount, nsUInt32 startVertex, nsUInt32 instanceCount)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!m_vertexSlice.buffer || m_vertexFrame != m_frameCounter)
      return;
    FlushDrawState();
    if (!m_renderingActive)
      return;
    VkCommandBuffer cmd = CurrentFrame().cmd;
    const VkDeviceSize off = m_vertexSlice.offset + m_vertexOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexSlice.buffer, &off);
    vkCmdDraw(cmd, vertexCount, std::max(1u, instanceCount), startVertex, 0);
  }

  void VulkanContext::DrawIndexed(
    nsUInt32 indexCount, nsUInt32 startIndex, nsInt32 baseVertex, nsUInt32 instanceCount)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!m_vertexSlice.buffer || !m_indexSlice.buffer || m_vertexFrame != m_frameCounter
      || m_indexFrame != m_frameCounter)
      return;
    FlushDrawState();
    if (!m_renderingActive)
      return;
    VkCommandBuffer cmd = CurrentFrame().cmd;
    const VkDeviceSize off = m_vertexSlice.offset + m_vertexOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexSlice.buffer, &off);
    // HarrlowVG::Index is 16-bit.
    vkCmdBindIndexBuffer(cmd, m_indexSlice.buffer, m_indexSlice.offset + m_indexOffset,
      VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(
      cmd, indexCount, std::max(1u, instanceCount), startIndex, baseVertex, 0);
  }

  // ==========================================================================
  // Textures
  // ==========================================================================

  VulkanContext::VulkanTexture* VulkanContext::CreateTextureRaw(
    uint32_t width, uint32_t height, VkFormat format, const void* data, uint32_t rowPitch)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_device == VK_NULL_HANDLE || width == 0 || height == 0)
      return nullptr;
    auto* tex = new VulkanTexture(this);
    if (!tex->Create(width, height, format))
    {
      delete tex;
      return nullptr;
    }
    if (data)
      UploadImage(tex, data, rowPitch ? rowPitch : width * tex->bytesPerPixel());
    else
    {
      // Give it a defined layout so it can be sampled or used as a target right away.
      EnsureFrameOpen();
      const bool wasActive = m_renderingActive;
      EndRendering();
      BeginRenderingTo(tex, true);
      EndRendering();
      if (wasActive)
        EnsureRenderingActive();
    }
    return tex;
  }

  nsObjectPtr<APHITexture> VulkanContext::CreateTexture(uint32_t width, uint32_t height,
    const nsString& format, const void* data, uint32_t rowPitch,
    const nsVariantDictionary&)
  {
    VulkanTexture* tex
      = CreateTextureRaw(width, height, FormatFromString(format), data, rowPitch);
    return tex ? nsObjectPtr<APHITexture>(tex) : nsObjectPtr<APHITexture>();
  }

  nsObjectPtr<APHIRenderTarget> VulkanContext::CreateRenderTarget(uint32_t width,
    uint32_t height, const nsString& colorFormat, const nsString&, const nsString&)
  {
    VulkanTexture* color
      = CreateTextureRaw(width, height, FormatFromString(colorFormat), nullptr, 0);
    if (!color)
      return { };
    return nsObjectPtr<APHIRenderTarget>(
      new VulkanRenderTarget(nsObjectPtr<VulkanTexture>(color), width, height));
  }

  void VulkanContext::UpdateTexture(
    APHITexture* texture, const void* data, uint32_t, uint32_t, uint32_t rowPitch)
  {
    if (texture && data)
      UploadImage(static_cast<VulkanTexture*>(texture), data, rowPitch);
  }

  void VulkanContext::UploadImage(VulkanTexture* tex, const void* pixels,
    uint32_t rowPitchBytes, uint32_t rowBegin, uint32_t rowEnd)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    EnsureFrameOpen();
    VkCommandBuffer cmd = CurrentFrame().cmd;
    const uint32_t bpp = tex->bytesPerPixel();
    const uint32_t w = tex->GetWidth();
    if (rowEnd == 0 || rowEnd > tex->GetHeight())
      rowEnd = tex->GetHeight();
    if (rowBegin >= rowEnd)
      return;
    const uint32_t h = rowEnd - rowBegin;
    const uint32_t tightRow = w * bpp;
    if (rowPitchBytes == 0)
      rowPitchBytes = tightRow;

    RingSlice staging = RingAlloc(static_cast<VkDeviceSize>(tightRow) * h, 16);
    if (!staging.buffer)
      return;
    const uint8_t* src = static_cast<const uint8_t*>(pixels)
      + static_cast<size_t>(rowBegin) * rowPitchBytes;
    uint8_t* dst = static_cast<uint8_t*>(staging.mapped);
    if (rowPitchBytes == tightRow)
      std::memcpy(dst, src, static_cast<size_t>(tightRow) * h);
    else
      for (uint32_t y = 0; y < h; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * tightRow,
          src + static_cast<size_t>(y) * rowPitchBytes, tightRow);

    EndRendering();
    VkImageMemoryBarrier b = { };
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout
      = tex->inGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->image();
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    tex->inGeneralLayout = true;

    VkBufferImageCopy region = { };
    region.bufferOffset = staging.offset;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, static_cast<int32_t>(rowBegin), 0 };
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(
      cmd, staging.buffer, tex->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    FullBarrier(cmd);
  }

  VulkanContext::VulkanTexture* VulkanContext::PrepareAtlasTexture(
    HarrlowVG::Atlas* atlas)
  {
    if (!atlas)
      return nullptr;
    const HarrlowVG::Vec2ui size = atlas->GetSize();
    if (size.x == 0 || size.y == 0)
      return nullptr;
    const bool isColor = atlas->IsColor();
    const VkFormat format = isColor ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM;

    AtlasEntry& entry = m_atlases[atlas];
    VulkanTexture* tex = entry.texture.get();
    if (!tex || tex->GetWidth() != size.x || tex->GetHeight() != size.y
      || tex->format() != format)
    {
      auto* fresh = new VulkanTexture(this);
      if (!fresh->Create(size.x, size.y, format))
      {
        delete fresh;
        return nullptr;
      }
      entry.texture.reset(fresh);
      entry.dirty = true;
      tex = fresh;
    }
    tex->isColorAtlas = isColor;
    if (entry.dirty && atlas->GetData())
    {
      const uint32_t stride = atlas->GetStride();
      UploadImage(tex, atlas->GetData(), stride ? stride : size.x * (isColor ? 4u : 1u));
      entry.dirty = false;
    }
    return tex;
  }

  void VulkanContext::OnAtlasUpdated(HarrlowVG::Atlas* atlas)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (atlas)
      m_atlases[atlas].dirty = true;
  }

  APHITexture* VulkanContext::ResolveExternalTextureHandle(
    HarrlowVG::TextureHandle handle)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!handle)
      return nullptr;
    if (VulkanTexture* tex = PrepareAtlasTexture(static_cast<HarrlowVG::Atlas*>(handle)))
      return tex;
    return SimpleAPHIRenderContext::ResolveExternalTextureHandle(handle);
  }

  APHITexture* VulkanContext::UploadCanvasSurface(
    uint64_t handle, const CanvasSurfaceView& surface)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!surface.pixels || surface.width == 0 || surface.height == 0)
      return nullptr;
    SurfaceEntry& entry = m_canvases[handle];
    entry.lastUsedFrame = m_frameCounter;
    VulkanTexture* tex = entry.texture.get();
    if (!tex || tex->GetWidth() != surface.width || tex->GetHeight() != surface.height)
    {
      auto* fresh = new VulkanTexture(this);
      if (!fresh->Create(surface.width, surface.height, VK_FORMAT_R8G8B8A8_UNORM))
      {
        delete fresh;
        return nullptr;
      }
      entry.texture.reset(fresh);
      entry.generation = surface.generation - 1;
      tex = fresh;
    }
    if (entry.generation != surface.generation || surface.dirty)
    {
      // Large first image uploads arrive a band of rows per frame; not drawn
      // until complete (SimpleAPHIRenderContext::PlanSurfaceUpload).
      uint32_t rowBegin = 0, rowEnd = surface.height;
      const bool complete = PlanSurfaceUpload(handle, surface, rowBegin, rowEnd);
      if (rowEnd > rowBegin)
        UploadImage(tex, surface.pixels, surface.stride, rowBegin, rowEnd);
      if (!complete)
        return nullptr;
      entry.generation = surface.generation;
      // The handle is opaque here: clear the owning bridge's dirty flag by
      // notifying every CanvasSurfaceView producer (each ignores handles it
      // does not own). Notifying the canvas bridge alone left decoded images
      // dirty forever, so every image re-uploaded on every frame.
      NotifyCanvasUploadComplete(handle, surface.generation);
      NotifySVGUploadComplete(handle, surface.generation);
      NotifyLottieUploadComplete(handle, surface.generation);
      NotifyImageUploadComplete(handle, surface.generation);
    }
    return tex;
  }

  APHITexture* VulkanContext::UploadVideoSurface(
    uint64_t handle, const VideoSurfaceView& surface)
  {
    if (!surface.yPlane || surface.width == 0 || surface.height == 0)
      return nullptr;
    SurfaceEntry& entry = m_videos[handle];
    entry.lastUsedFrame = m_frameCounter;
    VulkanTexture* tex = entry.texture.get();
    if (!tex || tex->GetWidth() != surface.width || tex->GetHeight() != surface.height)
    {
      auto* fresh = new VulkanTexture(this);
      if (!fresh->Create(surface.width, surface.height, VK_FORMAT_R8G8B8A8_UNORM))
      {
        delete fresh;
        return nullptr;
      }
      entry.texture.reset(fresh);
      entry.generation = surface.generation - 1;
      entry.rgba.resize(static_cast<size_t>(surface.width) * surface.height * 4);
      tex = fresh;
    }
    if (entry.generation != surface.generation || surface.dirty)
    {
      ConvertYUV420ToRGBA(surface, entry.rgba.data());
      UploadImage(tex, entry.rgba.data(), surface.width * 4);
      entry.generation = surface.generation;
      NotifyVideoUploadComplete(handle, surface.generation);
    }
    return tex;
  }

  void VulkanContext::EvictStaleSurfaces()
  {
    constexpr uint64_t kGrace = 300;
    auto sweep = [&](std::unordered_map<uint64_t, SurfaceEntry>& map)
    {
      for (auto it = map.begin(); it != map.end();)
      {
        if (it->second.lastUsedFrame + kGrace < m_frameCounter)
          it = map.erase(it);
        else
          ++it;
      }
    };
    sweep(m_canvases);
    sweep(m_videos);
  }

  // ==========================================================================
  // Render targets (layers)
  // ==========================================================================

  void VulkanContext::BindTargetTexture(
    APHITexture* texture, const HarrlowVG::Vec2& size, uintptr_t viewTag)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    EnsureFrameOpen();
    if (texture)
    {
      auto* tex = static_cast<VulkanTexture*>(texture);
      EndRendering();
      m_layerTarget = tex;
      // Clear on the layer's first bind (pooled RT); load when a layer that
      // was flushed mid-record (backdrop-filter) continues into it.
      BeginRenderingTo(tex, !BoundLayerTargetKeepsContents());
      m_scissor = { { 0, 0 }, { tex->GetWidth(), tex->GetHeight() } };
    }
    else if (m_layerTarget)
    {
      EndRendering();
      m_layerTarget = nullptr;
      m_scissor = { { 0, 0 }, m_swapchainExtent };
    }
    SimpleAPHIRenderContext::BindTargetTexture(texture, size, viewTag);
    m_viewportDirty = true;
  }

  void VulkanContext::ReleaseTargetTexture(APHITexture* texture)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (texture && m_layerTarget == static_cast<VulkanTexture*>(texture))
    {
      EndRendering();
      m_layerTarget = nullptr;
    }
    SimpleAPHIRenderContext::ReleaseTargetTexture(texture);
    m_viewportDirty = true;
  }

  void VulkanContext::CopyTexture(APHITexture* src, APHITexture* dst)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (!src || !dst || m_device == VK_NULL_HANDLE)
      return;
    auto* s = static_cast<VulkanTexture*>(src);
    auto* d = static_cast<VulkanTexture*>(dst);
    EnsureFrameOpen();
    const bool wasActive = m_renderingActive;
    EndRendering();
    VkCommandBuffer cmd = CurrentFrame().cmd;
    if (!d->inGeneralLayout)
    {
      BeginRenderingTo(d, true);
      EndRendering();
    }
    VkImageCopy region = { };
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = region.srcSubresource;
    region.extent = { std::min(s->GetWidth(), d->GetWidth()),
      std::min(s->GetHeight(), d->GetHeight()), 1 };
    vkCmdCopyImage(cmd, s->image(), VK_IMAGE_LAYOUT_GENERAL, d->image(),
      VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    FullBarrier(cmd);
    if (wasActive)
      EnsureRenderingActive();
  }

  void VulkanContext::ResolveTexture(APHITexture* msaaSrc, APHITexture* nonMsaaDst)
  {
    CopyTexture(msaaSrc, nonMsaaDst); // no MSAA in this backend: a plain copy
  }

  // ==========================================================================
  // Filter compute
  // ==========================================================================

  void VulkanContext::BindFilterComputeShader(FilterType type)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    EnsureFrameOpen();
    EndRendering();
    int key = static_cast<int>(type);
    if (type == FilterType::Blur)
      key = static_cast<int>(FilterType::BlurH);
    auto it = m_computePipelines.find(key);
    m_computePipeline = it != m_computePipelines.end() ? it->second : VK_NULL_HANDLE;
    m_computeIn0 = m_computeIn1 = m_computeOut = nullptr;
    m_computeUbo = RingSlice();
  }

  void VulkanContext::BindComputeInputSRV(APHITexture* inputTexture)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_computeIn0 = static_cast<VulkanTexture*>(inputTexture);
  }

  void VulkanContext::BindComputeInputSRV1(APHITexture* inputTexture)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_computeIn1 = static_cast<VulkanTexture*>(inputTexture);
  }

  void VulkanContext::BindComputeOutputUAV(APHITexture* outputTexture)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_computeOut = static_cast<VulkanTexture*>(outputTexture);
  }

  void VulkanContext::BindComputeConstantBuffer(uint32_t, APHIBuffer* buffer)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    auto* buf = static_cast<VulkanBuffer*>(buffer);
    if (!buf || !buf->IsConstant() || buf->cpu.empty())
      return;
    EnsureFrameOpen(); // the ring slice below belongs to the current frame
    m_computeUboSize = std::max<VkDeviceSize>(128, buf->cpu.size());
    m_computeUbo = RingAlloc(m_computeUboSize, 256);
    if (m_computeUbo.buffer)
    {
      std::memset(m_computeUbo.mapped, 0, static_cast<size_t>(m_computeUboSize));
      std::memcpy(m_computeUbo.mapped, buf->cpu.data(), buf->cpu.size());
    }
  }

  void VulkanContext::DoDispatchCompute(
    uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    if (m_computePipeline == VK_NULL_HANDLE || !m_computeIn0 || !m_computeOut
      || !m_computeUbo.buffer)
      return;
    // Another thread's frame boundary may have closed the frame BindFilterComputeShader()
    // opened; recording into a command buffer that was not begun crashes the driver.
    EnsureFrameOpen();
    EndRendering();
    Frame& f = CurrentFrame();
    VkCommandBuffer cmd = f.cmd;
    VulkanTexture* in1 = m_computeIn1 ? m_computeIn1 : m_computeIn0;

    VkDescriptorSetAllocateInfo alloc = { };
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = f.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_computeLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &alloc, &set) != VK_SUCCESS)
      return;
    const VkDescriptorBufferInfo ubo
      = { m_computeUbo.buffer, m_computeUbo.offset, m_computeUboSize };
    const VkDescriptorImageInfo images[4] = {
      { VK_NULL_HANDLE, m_computeIn0->view(), VK_IMAGE_LAYOUT_GENERAL },
      { VK_NULL_HANDLE, in1->view(), VK_IMAGE_LAYOUT_GENERAL },
      { VK_NULL_HANDLE, m_computeOut->view(), VK_IMAGE_LAYOUT_GENERAL },
      { m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED },
    };
    VkWriteDescriptorSet writes[5] = { };
    for (uint32_t i = 0; i < 5; ++i)
    {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &images[0];
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[2].pImageInfo = &images[1];
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = &images[2];
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[4].pImageInfo = &images[3];
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    FullBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout,
      0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
    FullBarrier(cmd);
  }

  void VulkanContext::UnbindComputeResources()
  {
    std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
    m_computePipeline = VK_NULL_HANDLE;
    m_computeIn0 = m_computeIn1 = m_computeOut = nullptr;
  }
} // namespace apui::samples
#endif
