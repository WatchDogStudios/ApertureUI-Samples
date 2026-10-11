// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "LogPanel.h"

#include <cstdio>
#include <ctime>
#include <imgui.h>

namespace apui::samples::player
{
  LogPanel::LogPanel()
  {
    m_logSubscription
      = nsGlobalLog::AddLogWriter(nsMakeDelegate(&LogPanel::OnLogMessage, this));
  }

  LogPanel::~LogPanel()
  {
    nsGlobalLog::RemoveLogWriter(m_logSubscription);
  }

  void LogPanel::OnLogMessage(const nsLoggingEventData& data)
  {
    uint8_t severity = 0;
    switch (data.m_EventType)
    {
      case nsLogMsgType::BeginGroup:
      case nsLogMsgType::EndGroup:
      case nsLogMsgType::None:
        return; // structure, not messages
      case nsLogMsgType::ErrorMsg:
      case nsLogMsgType::SeriousWarningMsg:
        severity = 2;
        break;
      case nsLogMsgType::WarningMsg:
        severity = 1;
        break;
      default:
        break;
    }

    const time_t t = time(nullptr);
    tm local = { };
    localtime_s(&local, &t);

    std::lock_guard<std::mutex> lock(m_mutex);
    LogEntry& e = m_ring[m_head];
    std::snprintf(e.timestamp, sizeof(e.timestamp), "%02d:%02d:%02d", local.tm_hour,
      local.tm_min, local.tm_sec);
    e.severity = severity;
    e.message.assign(data.m_sText.GetStartPointer(), data.m_sText.GetElementCount());
    m_head = (m_head + 1) % kCapacity;
    if (m_count < kCapacity)
      ++m_count;
    m_scrollToBottom = true;
  }

  void LogPanel::UpdateStats(float fps, float frameMs, size_t memoryMB)
  {
    m_fps = fps;
    m_frameMs = frameMs;
    m_memoryMB = memoryMB;
  }

  void LogPanel::Render()
  {
    ImGui::Begin("Log##panel");
    if (ImGui::BeginTabBar("##log_tabs"))
    {
      if (ImGui::BeginTabItem("Log"))
      {
        RenderLogTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Stats"))
      {
        RenderStatsTab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
  }

  void LogPanel::RenderLogTab()
  {
    auto severityToggle = [](const char* label, bool* v, ImVec4 onCol)
    {
      ImGui::PushStyleColor(ImGuiCol_FrameBg,
        *v ? ImVec4 { onCol.x * 0.25f, onCol.y * 0.25f, onCol.z * 0.25f, 1.0f }
           : ImVec4 { 0.10f, 0.11f, 0.14f, 1.0f });
      ImGui::PushStyleColor(ImGuiCol_CheckMark, onCol);
      ImGui::Checkbox(label, v);
      ImGui::PopStyleColor(2);
    };
    const ImVec4 infoCol = { 0.62f, 0.72f, 0.84f, 1.0f };
    const ImVec4 warnCol = { 0.98f, 0.78f, 0.30f, 1.0f };
    const ImVec4 errorCol = { 0.96f, 0.38f, 0.38f, 1.0f };
    severityToggle("Info", &m_showInfo, infoCol);
    ImGui::SameLine();
    severityToggle("Warn", &m_showWarn, warnCol);
    ImGui::SameLine();
    severityToggle("Error", &m_showError, errorCol);
    ImGui::SameLine();

    // Clear, right-aligned.
    const float clearW
      = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2 + 12;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - clearW + ImGui::GetCursorPosX());
    const bool clear = ImGui::Button("Clear");

    ImGui::Spacing();
    ImGui::Separator();

    // Copy the rows to draw while holding the lock, then let it go before ImGui draws
    // them: a thread that logs must never wait for the UI. m_visible keeps its strings'
    // storage from frame to frame, so a steady log copies without allocating.
    size_t visible = 0;
    bool scrollToBottom = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (clear)
      {
        m_head = 0;
        m_count = 0;
      }
      const int start = (m_count < kCapacity) ? 0 : m_head;
      for (int i = 0; i < m_count; ++i)
      {
        const LogEntry& e = m_ring[(start + i) % kCapacity];
        if ((e.severity == 0 && !m_showInfo) || (e.severity == 1 && !m_showWarn)
          || (e.severity == 2 && !m_showError))
          continue;
        if (visible == m_visible.size())
          m_visible.emplace_back();
        m_visible[visible++] = e;
      }
      scrollToBottom = m_scrollToBottom;
      m_scrollToBottom = false;
    }

    ImGui::BeginChild("##log_scroll", { 0, 0 }, ImGuiChildFlags_None,
      ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t i = 0; i < visible; ++i)
    {
      const LogEntry& e = m_visible[i];
      const ImVec4& col = e.severity == 2 ? errorCol
        : e.severity == 1                 ? warnCol
                                          : infoCol;
      const char* tag = e.severity == 2 ? "ERROR" : e.severity == 1 ? "WARN " : "INFO ";
      ImGui::TextDisabled("%s", e.timestamp);
      ImGui::SameLine(0, 8);
      ImGui::TextColored(col, "%s", tag);
      ImGui::SameLine(0, 8);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.87f, 0.90f, 1.0f));
      ImGui::TextUnformatted(e.message.c_str(), e.message.c_str() + e.message.size());
      ImGui::PopStyleColor();
    }
    if (scrollToBottom)
      ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
  }

  void LogPanel::RenderStatsTab()
  {
    m_fpsHistory[m_fpsOffset] = m_fps;
    m_fpsOffset = (m_fpsOffset + 1) % kFpsHistory;

    char fpsText[16];
    std::snprintf(fpsText, sizeof(fpsText), "%.0f fps", m_fps);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.290f, 0.690f, 1.000f, 1.0f));
    ImGui::TextUnformatted(fpsText);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("frame %.2f ms", m_frameMs);

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.290f, 0.690f, 1.000f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.063f, 0.071f, 0.090f, 1.0f));
    ImGui::PlotLines("##fps", m_fpsHistory.data(), kFpsHistory, m_fpsOffset, nullptr,
      0.0f, 200.0f, { ImGui::GetContentRegionAvail().x, 64.0f });
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    if (ImGui::BeginTable(
          "##stats_table", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg))
    {
      ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Value");
      auto row = [](const char* name, const char* value)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value);
      };
      char value[32];
      std::snprintf(value, sizeof(value), "%.2f ms", m_frameMs);
      row("Frame", value);
      std::snprintf(value, sizeof(value), "%.0f", m_fps);
      row("FPS", value);
      std::snprintf(value, sizeof(value), "%zu MB", m_memoryMB);
      row("Memory", value);
      ImGui::EndTable();
    }
  }
} // namespace apui::samples::player
