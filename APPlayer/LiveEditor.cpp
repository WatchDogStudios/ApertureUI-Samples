// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "LiveEditor.h"

#include <cstring>
#include <imgui.h>

namespace apui::samples::player
{
  namespace
  {
    constexpr size_t kInitialBufferSize = 4096;

    int VectorResize(ImGuiInputTextCallbackData* data)
    {
      if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
      {
        auto* vec = static_cast<std::vector<char>*>(data->UserData);
        vec->resize(static_cast<size_t>(data->BufSize));
        data->Buf = vec->data();
      }
      return 0;
    }

    void Fill(std::vector<char>& buf, const char* initial)
    {
      const size_t n = std::strlen(initial);
      buf.assign(n + kInitialBufferSize, '\0');
      std::memcpy(buf.data(), initial, n);
    }

    bool MultilineEdit(const char* id, std::vector<char>& buf, const ImVec2& size)
    {
      ImGui::PushID(id);
      const bool changed = ImGui::InputTextMultiline("##edit", buf.data(), buf.size(),
        size, ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
        VectorResize, &buf);
      ImGui::PopID();
      return changed;
    }
  } // namespace

  LiveEditor::LiveEditor()
  {
    // Starter content, so the editor shows something on first open.
    Fill(m_html,
      "<!-- HTML body -->\n"
      "<h1>Hello, Aperture</h1>\n"
      "<p class=\"intro\">Type in the panels and watch it render live.</p>\n"
      "<button id=\"btn\">Click me</button>\n");
    Fill(m_css,
      "/* CSS */\n"
      "body { font-family: sans-serif; padding: 24px; background:#0e1119; color:#e0e6ee; "
      "}\n"
      "h1   { color:#4ab0ff; margin-bottom:8px; }\n"
      ".intro { opacity:0.75; }\n"
      "#btn { padding:8px 16px; background:#4ab0ff; color:#000; border:0; "
      "border-radius:4px; "
      "cursor:pointer; }\n");
    Fill(m_js,
      "// JavaScript\n"
      "document.getElementById('btn').addEventListener('click', () => {\n"
      "  const el = document.querySelector('.intro');\n"
      "  el.textContent = 'Clicked at ' + new Date().toLocaleTimeString();\n"
      "});\n");
  }

  void LiveEditor::Apply()
  {
    if (!m_onApply)
      return;

    std::string doc;
    doc.reserve(m_html.size() + m_css.size() + m_js.size() + 256);
    doc += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    doc += "<title>Live Editor</title>\n<style>\n";
    doc += m_css.data();
    doc += "\n</style>\n</head>\n<body>\n";
    doc += m_html.data();
    doc += "\n<script>\n";
    doc += m_js.data();
    doc += "\n</script>\n</body>\n</html>\n";

    m_onApply(doc);
    m_dirty = false;
  }

  void LiveEditor::Render()
  {
    ImGui::Begin("Live Editor");

    ImGui::Checkbox("Auto-apply", &m_autoApply);
    ImGui::SameLine(0, 16);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.36f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.46f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.56f, 0.30f, 1.0f));
    if (ImGui::Button("  Apply  ", { 0, 0 }))
      Apply();
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (m_dirty)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.78f, 0.30f, 1.0f));
      ImGui::TextUnformatted("* unapplied changes");
      ImGui::PopStyleColor();
    }

    ImGui::Separator();

    const ImVec4 accent = { 0.290f, 0.690f, 1.000f, 1.0f };
    const ImVec2 paneSize(-1.0f, -1.0f);
    struct Tab
    {
      const char* name;
      const char* heading;
      const char* id;
      std::vector<char>* text;
    };
    const Tab tabs[] = {
      { "HTML", "HTML body", "html_edit", &m_html },
      { "CSS", "CSS", "css_edit", &m_css },
      { "JS", "JavaScript", "js_edit", &m_js },
    };
    if (ImGui::BeginTabBar("##live_tabs"))
    {
      for (const Tab& tab : tabs)
      {
        if (!ImGui::BeginTabItem(tab.name))
          continue;
        ImGui::Dummy({ 0, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(tab.heading);
        ImGui::PopStyleColor();
        if (MultilineEdit(tab.id, *tab.text, paneSize))
        {
          m_dirty = true;
          if (m_autoApply)
            Apply();
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::End();
  }
} // namespace apui::samples::player
