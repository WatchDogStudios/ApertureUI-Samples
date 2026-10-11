// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "Toolbar.h"

#include <cstdio>
#include <imgui.h>

namespace apui::samples::player
{
  void Toolbar::PushHistory(const std::string& url)
  {
    if (m_historyIndex + 1 < static_cast<int>(m_history.size()))
      m_history.erase(m_history.begin() + m_historyIndex + 1, m_history.end());
    m_history.push_back(url);
    m_historyIndex = static_cast<int>(m_history.size()) - 1;
  }

  void Toolbar::SetAddress(const std::string& url)
  {
    std::snprintf(m_urlBuf, sizeof(m_urlBuf), "%s", url.c_str());
  }

  void Toolbar::Navigate(const std::string& url)
  {
    if (url.empty())
      return;
    PushHistory(url);
    if (m_onNavigate)
      m_onNavigate(url);
  }

  void Toolbar::OnNavigated(const std::string& url)
  {
    SetAddress(url);
    if (m_historyIndex >= 0 && m_history[m_historyIndex] == url)
      return; // back, forward, reload, or a load this toolbar started
    PushHistory(url);
  }

  void Toolbar::UpdateStats(float fps, float frameMs, size_t memoryMB)
  {
    m_fps = fps;
    m_frameMs = frameMs;
    m_memoryMB = memoryMB;
  }

  void Toolbar::Render()
  {
    const ImVec4 accent = { 0.290f, 0.690f, 1.000f, 1.0f };
    const ImVec4 muted = { 0.080f, 0.090f, 0.115f, 1.0f };

    // Wordmark with an accent pill.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pillP0 = ImGui::GetCursorScreenPos();
    const ImVec2 pillP1 = { pillP0.x + 4.0f, pillP0.y + ImGui::GetFrameHeight() };
    dl->AddRectFilled(pillP0, pillP1, ImGui::ColorConvertFloat4ToU32(accent), 2.0f);
    ImGui::Dummy({ 6.0f, 0.0f });
    ImGui::SameLine(0, 6);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Aperture");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 14);

    // Back, forward, reload. The history index only moves on a click, so the
    // callbacks run after it is updated.
    auto navButton = [](const char* label, const char* tooltip, bool enabled)
    {
      if (!enabled)
        ImGui::BeginDisabled();
      const bool clicked = ImGui::Button(label, { 28, 0 });
      if (!enabled)
        ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tooltip);
      ImGui::SameLine(0, 4);
      return clicked;
    };
    const int count = static_cast<int>(m_history.size());
    int target = -1;
    if (navButton("<", "Back", m_historyIndex > 0))
      target = m_historyIndex - 1;
    if (navButton(">", "Forward", m_historyIndex + 1 < count))
      target = m_historyIndex + 1;
    if (navButton("R", "Reload", m_historyIndex >= 0))
      target = m_historyIndex;
    if (target >= 0)
    {
      m_historyIndex = target;
      const std::string url = m_history[target]; // the callback may change the history
      SetAddress(url);
      if (m_onNavigate)
        m_onNavigate(url);
    }

    ImGui::SameLine(0, 6);

    // Address field.
    const float statsW = 210.0f;
    const float watchW = 96.0f;
    const float aspectW = 120.0f; // fits "Aspect: 21:9" in the default font
    const float urlW
      = ImGui::GetContentRegionAvail().x - statsW - watchW - aspectW - 26.0f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, muted);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    ImGui::SetNextItemWidth(urlW > 80.0f ? urlW : 80.0f);
    if (ImGui::InputTextWithHint("##url", "File path or http(s) URL", m_urlBuf,
          sizeof(m_urlBuf), ImGuiInputTextFlags_EnterReturnsTrue))
      Navigate(m_urlBuf);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 10);

    // Watch toggle, green when on.
    const ImVec4 watchBg = m_watchActive ? ImVec4 { 0.10f, 0.36f, 0.18f, 1.0f }
                                         : ImVec4 { 0.20f, 0.13f, 0.13f, 1.0f };
    const ImVec4 watchHov = m_watchActive ? ImVec4 { 0.14f, 0.46f, 0.24f, 1.0f }
                                          : ImVec4 { 0.28f, 0.18f, 0.18f, 1.0f };
    ImGui::PushStyleColor(ImGuiCol_Button, watchBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, watchHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, watchHov);
    if (ImGui::Button(m_watchActive ? "Watch ON " : "Watch OFF", { watchW, 0 }))
    {
      m_watchActive = !m_watchActive;
      if (m_onWatch)
        m_onWatch(m_watchActive);
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 8);

    // Aspect-ratio lock: letterboxes the content viewport.
    static const char* const kAspectLabels[] = {
      "Aspect: Free",
      "Aspect: 16:9",
      "Aspect: 9:16",
      "Aspect: 4:3",
      "Aspect: 3:4",
      "Aspect: 1:1",
      "Aspect: 21:9",
    };
    constexpr int kAspectCount
      = static_cast<int>(sizeof(kAspectLabels) / sizeof(kAspectLabels[0]));
    if (m_aspectIndex < 0 || m_aspectIndex >= kAspectCount)
      m_aspectIndex = 0;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, muted);
    ImGui::SetNextItemWidth(aspectW);
    if (ImGui::BeginCombo(
          "##aspect", kAspectLabels[m_aspectIndex], ImGuiComboFlags_HeightLargest))
    {
      for (int i = 0; i < kAspectCount; ++i)
      {
        const bool selected = (m_aspectIndex == i);
        if (ImGui::Selectable(kAspectLabels[i], selected))
        {
          m_aspectIndex = i;
          if (m_onAspect)
            m_onAspect(i);
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 14);

    // Frame rate, frame time, process working set.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%.0f fps   %.1f ms   %zu MB", m_fps, m_frameMs, m_memoryMB);
  }
} // namespace apui::samples::player
