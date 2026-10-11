// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// LogPanel.h - the "Log" panel: the engine's log (the last kCapacity messages, filtered
// by severity) and a "Stats" tab with the frame rate history.

#pragma once

#include <Foundation/Logging/Log.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace apui::samples::player
{
  class LogPanel
  {
  public:
    /// Starts receiving log messages; the destructor stops.
    LogPanel();
    ~LogPanel();
    LogPanel(const LogPanel&) = delete;
    LogPanel& operator=(const LogPanel&) = delete;

    void Render();
    void UpdateStats(float fps, float frameMs, size_t memoryMB);

  private:
    struct LogEntry
    {
      char timestamp[12];
      uint8_t severity; // 0 info, 1 warning, 2 error
      std::string message;
    };

    void OnLogMessage(const nsLoggingEventData& data);
    void RenderLogTab();
    void RenderStatsTab();

    static constexpr int kCapacity = 1000;
    static constexpr int kFpsHistory = 120;

    // Log messages arrive on any thread.
    std::mutex m_mutex;
    std::array<LogEntry, kCapacity> m_ring;
    int m_head = 0;
    int m_count = 0;
    std::vector<LogEntry> m_visible; // the rows drawn this frame (render thread only)
    bool m_scrollToBottom = true;
    nsEventSubscriptionID m_logSubscription = 0;

    bool m_showInfo = true;
    bool m_showWarn = true;
    bool m_showError = true;

    float m_fps = 0.0f;
    float m_frameMs = 0.0f;
    size_t m_memoryMB = 0;
    std::array<float, kFpsHistory> m_fpsHistory = { };
    int m_fpsOffset = 0;
  };
} // namespace apui::samples::player
