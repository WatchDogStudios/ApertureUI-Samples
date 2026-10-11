// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// IPlayerRenderContext.h - the render backend interface the samples' hosts drive.

#pragma once

#include <APHarrlow/RenderPath/Interfaces/HarrlowCommandList.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Types/Types.h>

namespace aperture::harrlow::renderpath
{
  class APHIRenderContext;
}

#if NS_ENABLED(NS_PLATFORM_WINDOWS)
#  include <windows.h>
#endif

namespace HarrlowVG
{
  class Atlas;
}

namespace apui::samples
{
  /// Lightweight interface exposing only the methods a host needs.
  /// Both D3D11 and Vulkan contexts implement this alongside SimpleAPHIRenderContext.
  class IPlayerRenderContext
  {
  public:
    virtual ~IPlayerRenderContext() = default;

    virtual bool Initialize(HWND windowHandle, nsUInt32 width, nsUInt32 height) = 0;
    virtual void Shutdown() = 0;

    virtual void BeginFrame(const nsColor& clearColor) = 0;
    virtual void RenderCommands(
      const aperture::harrlow::renderpath::HarrlowCommandList& list) = 0;
    virtual void SubmitCommands() = 0;
    virtual void EndFrame(bool enableVSync) = 0;
    virtual void Resize(nsUInt32 width, nsUInt32 height) = 0;

    virtual void OnAtlasUpdated(HarrlowVG::Atlas* atlas) = 0;

    /// The APHIRenderContext side of this context (both contexts also
    /// derive SimpleAPHIRenderContext). Handed to UIView::setRenderPathContext
    /// so the paint pipeline can acquire layer scratch RTs for CSS group
    /// effects (opacity groups, blend isolation, filters, masks). The engine
    /// is built /GR- (no dynamic_cast), hence an explicit virtual upcast.
    /// Default null = backend doesn't support layer compositing.
    virtual aperture::harrlow::renderpath::APHIRenderContext* GetRenderPathContext()
    {
      return nullptr;
    }

    /// Request a one-shot backbuffer dump (24-bit BMP) during the next
    /// EndFrame, just before Present. Lets a host grab a live frame after
    /// N frames of continuous animation, which shows animation and
    /// compositing problems that a single-frame render does not.
    virtual void RequestBackbufferCapture(const char* /*bmpPath*/) { }

    /// Sets a screen-space offset applied to the D3D11/Vulkan viewport and scissor
    /// rect during rendering.  Shader constants (viewportOrigin / viewportSize) are
    /// left untouched so that the coordinate transform remains correct while content
    /// is composited at an arbitrary screen position.  Use this to render multiple
    /// UIView command lists to different regions of the same back-buffer within a
    /// single frame.
    virtual void SetViewportScreenOffset(float x, float y) = 0;

    /// Enable or disable render-to-texture mode.
    /// When enabled, frames are rendered into an offscreen render target instead of
    /// the swapchain back-buffer. The resulting texture can be read back or composited
    /// by an external engine.
    virtual void SetRenderToTexture(bool enable) { m_renderToTexture = enable; }
    bool IsRenderToTexture() const { return m_renderToTexture; }

    /// Returns the shader resource view for the offscreen RTT texture (D3D11 only).
    /// Returns nullptr if RTT is not active or on non-D3D11 backends.
    virtual void* GetContentSRV() { return nullptr; }

    /// Resizes the offscreen RTT texture. No-op if RTT is not active.
    virtual void ResizeContent(nsUInt32 width, nsUInt32 height)
    {
      (void)width;
      (void)height;
    }

    /// Switches the D3D11 OM render target to the swapchain backbuffer and clears it.
    /// Call this between SubmitCommands() and any ImGui rendering so that ImGui draws
    /// to the backbuffer instead of the offscreen RTT.  The RTT blit in EndFrame() is
    /// skipped automatically when this has been called (ImGui composites the RTT via
    /// ImGui::Image()).  No-op on non-D3D11 backends.
    virtual void BindBackbuffer(const nsColor& clearColor) { (void)clearColor; }

  protected:
    bool m_renderToTexture = false;
  };

} // namespace apui::samples
