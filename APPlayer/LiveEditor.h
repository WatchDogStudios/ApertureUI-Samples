// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// LiveEditor.h - a three-tab HTML/CSS/JS editor whose "Apply" loads the combined
// document into the content viewport.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace apui::samples::player
{
  class LiveEditor
  {
  public:
    using ApplyCallback = std::function<void(const std::string& html)>;

    LiveEditor();

    void SetApplyCallback(ApplyCallback cb) { m_onApply = std::move(cb); }

    void Render();

    /// Builds the combined document (the CSS in a <style>, the HTML as the <body>, the
    /// JS in a <script>) and passes it to the apply callback.
    void Apply();

  private:
    // ImGui::InputTextMultiline needs contiguous, resizable storage; it grows these
    // through a resize callback.
    std::vector<char> m_html;
    std::vector<char> m_css;
    std::vector<char> m_js;

    ApplyCallback m_onApply;
    bool m_autoApply = true; // re-apply on every edit
    bool m_dirty = false;
  };
} // namespace apui::samples::player
