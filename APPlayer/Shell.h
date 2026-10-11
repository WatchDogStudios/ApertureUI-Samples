// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// Shell.h - APPlayer's Dear ImGui shell (D3D11 only): the toolbar, a dock space with
// the File Browser, Inspector, Live Editor and Log panels, and the "Content" panel
// that shows the page's render target.

#pragma once

#include <Foundation/Basics.h>

#include <imgui.h>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct SDL_Window;

namespace apui::samples::player
{
  class Toolbar;
  class FileBrowser;
  class LogPanel;
  class Inspector;
  class LiveEditor;
  class State;

  /// Aspect ratios the content viewport can be locked to, in the toolbar's order.
  enum class AspectRatio
  {
    Free = 0, // fill the panel
    Sixteen9,
    Nine16,
    Four3,
    Three4,
    Square,
    TwentyOne9,
  };

  class Shell
  {
  public:
    struct Config
    {
      ID3D11Device* device = nullptr;
      ID3D11DeviceContext* deviceContext = nullptr;
      SDL_Window* window = nullptr;
      State* state = nullptr;
      std::string defaultWorkspace; // UTF-8 folder the File Browser shows first
      std::string iniPath;          // UTF-8 file for the dock layout; empty for none
    };

    Shell();
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    /// Creates the ImGui context and its SDL2 and D3D11 back ends. False when a back end
    /// fails; the shell is then unusable and Shutdown (or the destructor) cleans up.
    bool Initialize(const Config& cfg);
    void Shutdown();

    /// Draws one frame into the bound render target. contentSRV is the page's render
    /// target (an ID3D11ShaderResourceView), sized to windowSize.
    void Render(void* contentSRV, ImVec2 windowSize);

    ImVec2 GetContentViewportPos() const { return m_contentPos; }
    ImVec2 GetContentViewportSize() const { return m_contentSize; }

    Toolbar* GetToolbar() { return m_toolbar.get(); }
    FileBrowser* GetFileBrowser() { return m_fileBrowser.get(); }
    LogPanel* GetLogPanel() { return m_logPanel.get(); }
    Inspector* GetInspector() { return m_inspector.get(); }
    LiveEditor* GetLiveEditor() { return m_liveEditor.get(); }

  private:
    void ApplyStyle();
    void SetupDefaultLayout(ImGuiID dockSpaceId, ImVec2 viewSize);
    void RenderContentWindow(void* srv, ImVec2 windowSize);
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    void RenderDebugOverlay();
#endif

    bool m_contextCreated = false;
    bool m_sdlBackend = false;
    bool m_dx11Backend = false;
    std::string m_iniPath; // ImGuiIO::IniFilename points into this

    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<FileBrowser> m_fileBrowser;
    std::unique_ptr<LogPanel> m_logPanel;
    std::unique_ptr<Inspector> m_inspector;
    std::unique_ptr<LiveEditor> m_liveEditor;

    ImVec2 m_contentPos = { 0, 0 };
    ImVec2 m_contentSize = { 1280, 720 };
    float m_toolbarHeight = 34.0f;
    bool m_firstRun = true;

    AspectRatio m_aspect = AspectRatio::Free;
  };
} // namespace apui::samples::player
