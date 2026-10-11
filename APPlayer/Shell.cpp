// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "Shell.h"

#include "FileBrowser.h"
#include "Inspector.h"
#include "LiveEditor.h"
#include "LogPanel.h"
#include "Toolbar.h"

#include <APHTML/Paint/V2/DebugOverlay.h>

#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h>

namespace apui::samples::player
{
  Shell::Shell() = default;

  Shell::~Shell()
  {
    Shutdown();
  }

  bool Shell::Initialize(const Config& cfg)
  {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_contextCreated = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // The dock layout is kept where the caller says, not in the working folder. ImGui
    // keeps the pointer, so the string is a member.
    m_iniPath = cfg.iniPath;
    io.IniFilename = m_iniPath.empty() ? nullptr : m_iniPath.c_str();
    ApplyStyle();

    m_sdlBackend = ImGui_ImplSDL2_InitForD3D(cfg.window);
    m_dx11Backend = m_sdlBackend && ImGui_ImplDX11_Init(cfg.device, cfg.deviceContext);
    if (!m_dx11Backend)
      return false;

    m_toolbar = std::make_unique<Toolbar>();
    m_fileBrowser = std::make_unique<FileBrowser>();
    m_logPanel = std::make_unique<LogPanel>();
    m_inspector = std::make_unique<Inspector>();
    m_liveEditor = std::make_unique<LiveEditor>();

    m_fileBrowser->SetState(cfg.state, cfg.defaultWorkspace);
    m_toolbar->SetAspectRatio(static_cast<int>(m_aspect));
    m_toolbar->SetAspectChangedCallback(
      [this](int idx) { m_aspect = static_cast<AspectRatio>(idx); });
    return true;
  }

  void Shell::Shutdown()
  {
    m_liveEditor.reset();
    m_inspector.reset();
    m_logPanel.reset();
    m_fileBrowser.reset();
    m_toolbar.reset();
    if (m_dx11Backend)
      ImGui_ImplDX11_Shutdown();
    if (m_sdlBackend)
      ImGui_ImplSDL2_Shutdown();
    if (m_contextCreated)
      ImGui::DestroyContext();
    m_dx11Backend = m_sdlBackend = m_contextCreated = false;
  }

  void Shell::ApplyStyle()
  {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry: generous rounding, no borders.
    s.WindowRounding = 6.0f;
    s.ChildRounding = 5.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 5.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBorderSize = 0.0f;
    s.WindowPadding = { 10, 10 };
    s.FramePadding = { 9, 5 };
    s.CellPadding = { 6, 4 };
    s.ItemSpacing = { 8, 6 };
    s.ItemInnerSpacing = { 6, 5 };
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 14.0f;
    s.WindowTitleAlign = { 0.0f, 0.5f };
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = { 0.0f, 0.5f };
    s.SeparatorTextPadding = { 18.0f, 3.0f };

    // Palette: deep ink-blue background, slate panels, cyan accent, off-white text;
    // lower contrast than black and white, for long sessions.
    const ImVec4 bg0 = { 0.043f, 0.055f, 0.078f, 1.00f }; // deepest: toolbar, popups
    const ImVec4 bg1 = { 0.075f, 0.090f, 0.118f, 1.00f }; // panel
    const ImVec4 bg2 = { 0.118f, 0.140f, 0.180f, 1.00f }; // raised control
    const ImVec4 bg3 = { 0.165f, 0.200f, 0.260f, 1.00f }; // hovered control
    const ImVec4 accent = { 0.310f, 0.720f, 1.000f, 1.00f };
    const ImVec4 accentDim = { 0.215f, 0.500f, 0.795f, 1.00f };
    const ImVec4 accentSoft = { accent.x, accent.y, accent.z, 0.22f };
    const ImVec4 textPrimary = { 0.905f, 0.925f, 0.955f, 1.00f };
    const ImVec4 textDim = { 0.540f, 0.605f, 0.685f, 1.00f };

    s.Colors[ImGuiCol_Text] = textPrimary;
    s.Colors[ImGuiCol_TextDisabled] = textDim;
    s.Colors[ImGuiCol_WindowBg] = bg1;
    s.Colors[ImGuiCol_ChildBg] = bg1;
    s.Colors[ImGuiCol_PopupBg] = bg0;
    s.Colors[ImGuiCol_Border] = { 0, 0, 0, 0 };
    s.Colors[ImGuiCol_BorderShadow] = { 0, 0, 0, 0 };
    s.Colors[ImGuiCol_FrameBg] = bg0;
    s.Colors[ImGuiCol_FrameBgHovered] = bg2;
    s.Colors[ImGuiCol_FrameBgActive] = bg3;
    s.Colors[ImGuiCol_TitleBg] = bg0;
    s.Colors[ImGuiCol_TitleBgActive] = bg0;
    s.Colors[ImGuiCol_TitleBgCollapsed] = bg0;
    s.Colors[ImGuiCol_MenuBarBg] = bg0;
    s.Colors[ImGuiCol_ScrollbarBg] = { 0, 0, 0, 0 };
    s.Colors[ImGuiCol_ScrollbarGrab] = bg2;
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = bg3;
    s.Colors[ImGuiCol_ScrollbarGrabActive] = accentDim;
    s.Colors[ImGuiCol_CheckMark] = accent;
    s.Colors[ImGuiCol_SliderGrab] = accent;
    s.Colors[ImGuiCol_SliderGrabActive] = accent;
    s.Colors[ImGuiCol_Button] = bg2;
    s.Colors[ImGuiCol_ButtonHovered] = bg3;
    s.Colors[ImGuiCol_ButtonActive] = accentDim;
    s.Colors[ImGuiCol_Header] = bg2;
    s.Colors[ImGuiCol_HeaderHovered] = bg3;
    s.Colors[ImGuiCol_HeaderActive] = accentDim;
    s.Colors[ImGuiCol_Separator] = bg2;
    s.Colors[ImGuiCol_SeparatorHovered] = accentDim;
    s.Colors[ImGuiCol_SeparatorActive] = accent;
    s.Colors[ImGuiCol_ResizeGrip] = { 0, 0, 0, 0 };
    s.Colors[ImGuiCol_ResizeGripHovered] = accentSoft;
    s.Colors[ImGuiCol_ResizeGripActive] = accent;
    s.Colors[ImGuiCol_Tab] = bg1;
    s.Colors[ImGuiCol_TabHovered] = bg3;
    s.Colors[ImGuiCol_TabSelected] = bg2;
    s.Colors[ImGuiCol_TabDimmed] = bg1;
    s.Colors[ImGuiCol_TabDimmedSelected] = bg2;
    s.Colors[ImGuiCol_DockingPreview] = accentSoft;
    s.Colors[ImGuiCol_DockingEmptyBg] = bg0;
    s.Colors[ImGuiCol_PlotLines] = accent;
    s.Colors[ImGuiCol_PlotLinesHovered] = textPrimary;
    s.Colors[ImGuiCol_PlotHistogram] = accent;
    s.Colors[ImGuiCol_PlotHistogramHovered] = textPrimary;
    s.Colors[ImGuiCol_TableHeaderBg] = bg2;
    s.Colors[ImGuiCol_TableBorderStrong] = bg2;
    s.Colors[ImGuiCol_TableBorderLight] = bg1;
    s.Colors[ImGuiCol_TableRowBg] = { 0, 0, 0, 0 };
    s.Colors[ImGuiCol_TableRowBgAlt] = { 1, 1, 1, 0.025f };
    s.Colors[ImGuiCol_TextSelectedBg] = accentSoft;
    s.Colors[ImGuiCol_DragDropTarget] = accent;
    s.Colors[ImGuiCol_NavCursor] = accent;
    s.Colors[ImGuiCol_NavWindowingHighlight] = textPrimary;
    s.Colors[ImGuiCol_NavWindowingDimBg] = { 0, 0, 0, 0.50f };
    s.Colors[ImGuiCol_ModalWindowDimBg] = { 0, 0, 0, 0.45f };
  }

  void Shell::Render(void* contentSRV, ImVec2 windowSize)
  {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Fixed toolbar.
    ImGui::SetNextWindowPos({ 0.0f, 0.0f });
    ImGui::SetNextWindowSize({ windowSize.x, m_toolbarHeight });
    constexpr ImGuiWindowFlags kToolbarFlags = ImGuiWindowFlags_NoTitleBar
      | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking
      | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##toolbar", nullptr, kToolbarFlags);
    m_toolbar->Render();
    m_toolbarHeight = ImGui::GetWindowHeight();
    ImGui::End();

    // Dock space under the toolbar.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dockY = m_toolbarHeight;
    ImGui::SetNextWindowPos({ vp->Pos.x, vp->Pos.y + dockY });
    ImGui::SetNextWindowSize({ vp->Size.x, vp->Size.y - dockY });
    ImGui::SetNextWindowViewport(vp->ID);
    constexpr ImGuiWindowFlags kDockFlags = ImGuiWindowFlags_NoTitleBar
      | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
      | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::Begin("##ds_host", nullptr, kDockFlags);
    ImGui::PopStyleVar();

    const ImGuiID dockSpaceId = ImGui::GetID("ApertureDockSpace");
    ImGui::DockSpace(dockSpaceId, { 0, 0 }, ImGuiDockNodeFlags_PassthruCentralNode);
    if (m_firstRun)
    {
      SetupDefaultLayout(dockSpaceId, { vp->Size.x, vp->Size.y - dockY });
      m_firstRun = false;
    }
    ImGui::End();

    m_fileBrowser->Render();
    m_inspector->Render();
    m_logPanel->Render();
    m_liveEditor->Render();
    RenderContentWindow(contentSRV, windowSize);

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }

  void Shell::SetupDefaultLayout(ImGuiID dockSpaceId, ImVec2 viewSize)
  {
    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, viewSize);

    ImGuiID center = dockSpaceId;
    ImGuiID left, right, bottom;
    ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Left, 220.0f / viewSize.x, &left, &center);
    ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Right, 260.0f / (viewSize.x - 220.0f), &right, &center);
    ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Down, 160.0f / viewSize.y, &bottom, &center);

    ImGui::DockBuilderDockWindow("File Browser", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Live Editor", right); // a tab beside the Inspector
    ImGui::DockBuilderDockWindow("Log##panel", bottom);
    ImGui::DockBuilderDockWindow("Content", center);
    ImGui::DockBuilderFinish(dockSpaceId);
  }

  void Shell::RenderContentWindow(void* srv, ImVec2 windowSize)
  {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::Begin("Content");

    const ImVec2 panelPos = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetContentRegionAvail();

    float targetAspect = 0.0f;
    switch (m_aspect)
    {
      case AspectRatio::Sixteen9:
        targetAspect = 16.0f / 9.0f;
        break;
      case AspectRatio::Nine16:
        targetAspect = 9.0f / 16.0f;
        break;
      case AspectRatio::Four3:
        targetAspect = 4.0f / 3.0f;
        break;
      case AspectRatio::Three4:
        targetAspect = 3.0f / 4.0f;
        break;
      case AspectRatio::Square:
        targetAspect = 1.0f;
        break;
      case AspectRatio::TwentyOne9:
        targetAspect = 21.0f / 9.0f;
        break;
      case AspectRatio::Free:
        break;
    }

    // The content area: the whole panel, or letterboxed to the locked aspect ratio.
    ImVec2 contentArea = panelSize;
    ImVec2 contentOffset = { 0.0f, 0.0f };
    if (targetAspect > 0.0f && panelSize.x > 1.0f && panelSize.y > 1.0f)
    {
      if (panelSize.x / panelSize.y > targetAspect)
      {
        contentArea.x = panelSize.y * targetAspect; // bars left and right
        contentOffset.x = (panelSize.x - contentArea.x) * 0.5f;
      }
      else
      {
        contentArea.y = panelSize.x / targetAspect; // bars top and bottom
        contentOffset.y = (panelSize.y - contentArea.y) * 0.5f;
      }
      ImGui::GetWindowDrawList()->AddRectFilled(panelPos,
        { panelPos.x + panelSize.x, panelPos.y + panelSize.y },
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.020f, 0.024f, 0.035f, 1.0f)));
    }

    // Input routing and the view's viewport follow the content area, not the panel.
    m_contentPos = { panelPos.x + contentOffset.x, panelPos.y + contentOffset.y };
    m_contentSize = contentArea;

    if (srv && contentArea.x > 1.0f && contentArea.y > 1.0f)
    {
      // The render target is window-sized; crop it to the content area so the page is
      // sampled 1:1, never stretched.
      ImVec2 uv1 = { 1.0f, 1.0f };
      if (windowSize.x > 0.0f && windowSize.y > 0.0f)
        uv1 = { contentArea.x / windowSize.x, contentArea.y / windowSize.y };

      ImGui::SetCursorScreenPos(m_contentPos);
      ImGui::Image(reinterpret_cast<ImTextureID>(srv), contentArea, { 0.0f, 0.0f }, uv1);
      m_inspector->DrawHighlight(ImGui::GetWindowDrawList(), m_contentPos);
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
      RenderDebugOverlay();
#endif
    }

    ImGui::End();
    ImGui::PopStyleVar();
  }

#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
  void Shell::RenderDebugOverlay()
  {
    auto* dbg = aperture::paint::DebugOverlay::Active();
    if (!dbg || dbg->Primitives().empty())
      return;

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImVec2 origin = m_contentPos;
    auto toScreen = [&](aperture::paint::Vec2 p) -> ImVec2
    { return { origin.x + p.x, origin.y + p.y }; };
    auto col = [](aperture::paint::Color c) -> ImU32
    { return ImGui::ColorConvertFloat4ToU32({ c.r, c.g, c.b, c.a }); };

    for (const auto& p : dbg->Primitives())
    {
      switch (p.kind)
      {
        case aperture::paint::DebugPrimitive::Kind::Rect:
        {
          const ImVec2 tl = toScreen({ p.rect.r.x, p.rect.r.y });
          const ImVec2 br = toScreen({ p.rect.r.Right(), p.rect.r.Bottom() });
          if (p.rect.filled)
            fg->AddRectFilled(tl, br, col(p.rect.color));
          else
            fg->AddRect(tl, br, col(p.rect.color), 0.f, 0, p.rect.thickness);
          break;
        }
        case aperture::paint::DebugPrimitive::Kind::Line:
          fg->AddLine(
            toScreen(p.line.a), toScreen(p.line.b), col(p.line.color), p.line.thickness);
          break;
        case aperture::paint::DebugPrimitive::Kind::Text:
          fg->AddText(toScreen(p.text.pos), col(p.text.color), p.text.text);
          break;
        case aperture::paint::DebugPrimitive::Kind::Circle:
          if (p.circle.filled)
            fg->AddCircleFilled(
              toScreen(p.circle.center), p.circle.radius, col(p.circle.color));
          else
            fg->AddCircle(
              toScreen(p.circle.center), p.circle.radius, col(p.circle.color));
          break;
      }
    }
  }
#endif
} // namespace apui::samples::player
