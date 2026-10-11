// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "FileBrowser.h"

#include "State.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <imgui.h>
#include <nfd.hpp>

namespace fs = std::filesystem;

namespace apui::samples::player
{
  namespace
  {
    bool IsHtml(const fs::path& p)
    {
      const std::wstring ext = p.extension().wstring();
      return _wcsicmp(ext.c_str(), L".html") == 0 || _wcsicmp(ext.c_str(), L".htm") == 0;
    }

    std::string BaseName(const std::string& path)
    {
      const size_t slash = path.find_last_of("/\\");
      return slash == std::string::npos ? path : path.substr(slash + 1);
    }
  } // namespace

  void FileBrowser::SetState(State* state, std::string defaultRoot)
  {
    m_state = state;
    SetRoot(state && !state->workspaceRoot.empty() ? state->workspaceRoot
                                                   : std::move(defaultRoot));
  }

  void FileBrowser::SetRoot(std::string root)
  {
    m_root = std::move(root);
    m_listings.clear();
  }

  void FileBrowser::TryLoad(const std::string& path)
  {
    m_selectedPath = path;
    if (m_onLoad)
      m_onLoad(path);
  }

  void FileBrowser::Render()
  {
    ImGui::Begin("File Browser");
    if (ImGui::BeginTabBar("##fb_tabs"))
    {
      if (ImGui::BeginTabItem("Files"))
      {
        RenderFilesTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Recent"))
      {
        RenderRecentTab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
  }

  void FileBrowser::RenderFilesTab()
  {
    if (m_root.empty())
    {
      ImGui::TextDisabled("No workspace folder set");
    }
    else
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.72f, 0.84f, 1.0f));
      ImGui::TextWrapped("%s", m_root.c_str());
      ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const float half
      = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Choose folder...", { half, 0 }))
    {
      NFD::Guard guard; // the dialog's COM initialisation, for this call only
      NFD::UniquePath out;
      if (NFD::PickFolder(out) == NFD_OKAY)
      {
        SetRoot(out.get());
        if (m_state)
          m_state->workspaceRoot = m_root;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", { -FLT_MIN, 0 }))
      m_listings.clear();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!m_root.empty())
      RenderDirectory(m_root);
  }

  const std::vector<FileBrowser::Entry>& FileBrowser::List(const std::string& dir)
  {
    auto found = m_listings.find(dir);
    if (found != m_listings.end())
      return found->second;

    std::vector<Entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end;
      it.increment(ec))
    {
      const bool isDirectory = it->is_directory(ec);
      if (ec)
        break;
      if (isDirectory || IsHtml(it->path()))
        entries.push_back(
          { it->path().filename().u8string(), it->path().u8string(), isDirectory });
    }
    std::sort(entries.begin(), entries.end(),
      [](const Entry& a, const Entry& b)
      {
        if (a.isDirectory != b.isDirectory)
          return a.isDirectory;
        return a.name < b.name;
      });
    return m_listings.emplace(dir, std::move(entries)).first->second;
  }

  void FileBrowser::RenderDirectory(const std::string& dir)
  {
    // Listing a subfolder adds to m_listings; that never moves this listing (an
    // unordered_map keeps references to its values valid when it rehashes).
    for (const Entry& entry : List(dir))
    {
      if (entry.isDirectory)
      {
        if (ImGui::TreeNodeEx(entry.path.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
              entry.name.c_str()))
        {
          RenderDirectory(entry.path);
          ImGui::TreePop();
        }
        continue;
      }
      ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
        | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
      if (m_selectedPath == entry.path)
        flags |= ImGuiTreeNodeFlags_Selected;
      ImGui::TreeNodeEx(entry.path.c_str(), flags, "%s", entry.name.c_str());
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        TryLoad(entry.path);
    }
  }

  void FileBrowser::RenderRecentTab()
  {
    if (!m_state || m_state->recentFiles.empty())
    {
      ImGui::TextDisabled("Nothing opened yet");
      return;
    }
    int idx = 0;
    for (const std::string& r : m_state->recentFiles)
    {
      const std::string name = BaseName(r); // the full path is in the tooltip
      ImGui::PushID(idx++);
      if (ImGui::Selectable(
            name.c_str(), m_selectedPath == r, ImGuiSelectableFlags_AllowDoubleClick)
        && ImGui::IsMouseDoubleClicked(0))
      {
        const std::string path = r; // loading reorders the recent list
        ImGui::PopID();
        TryLoad(path);
        return;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", r.c_str());
      ImGui::PopID();
    }
  }
} // namespace apui::samples::player
