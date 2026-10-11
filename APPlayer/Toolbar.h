// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// Toolbar.h - the bar across the top of APPlayer: back, forward, reload, the address
// field, the file-watch toggle, the aspect-ratio lock and the frame statistics.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace apui::samples::player
{
  class Toolbar
  {
  public:
    using NavigateCallback = std::function<void(const std::string& url)>;
    using WatchCallback = std::function<void(bool active)>;
    using AspectChangedCallback = std::function<void(int aspectIndex)>;

    void SetNavigateCallback(NavigateCallback cb) { m_onNavigate = std::move(cb); }
    void SetWatchCallback(WatchCallback cb) { m_onWatch = std::move(cb); }
    void SetAspectChangedCallback(AspectChangedCallback cb)
    {
      m_onAspect = std::move(cb);
    }

    void Render();
    /// Records a document the host loaded, for back, forward and the address field.
    void OnNavigated(const std::string& url);
    void SetWatchActive(bool active) { m_watchActive = active; }
    void UpdateStats(float fps, float frameMs, size_t memoryMB);

    void SetAspectRatio(int idx) { m_aspectIndex = idx; }

  private:
    void Navigate(const std::string& url);
    void PushHistory(const std::string& url);
    void SetAddress(const std::string& url);

    std::vector<std::string> m_history;
    int m_historyIndex = -1;
    char m_urlBuf[2048] = { };
    bool m_watchActive = false;
    float m_fps = 0.0f;
    float m_frameMs = 0.0f;
    size_t m_memoryMB = 0;
    int m_aspectIndex = 0; // AspectRatio order, see Shell.h

    NavigateCallback m_onNavigate;
    WatchCallback m_onWatch;
    AspectChangedCallback m_onAspect;
  };
} // namespace apui::samples::player
