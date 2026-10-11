// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// FileBrowser.h - the "File Browser" panel: a tree of the HTML files under a workspace
// folder (the executable's Data folder until one is chosen) and the recently opened
// documents. Double-click loads a document.

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace apui::samples::player
{
  class State;

  class FileBrowser
  {
  public:
    using LoadCallback = std::function<void(const std::string& path)>;

    void SetLoadCallback(LoadCallback cb) { m_onLoad = std::move(cb); }
    /// state: where the workspace folder and the recent list are kept. defaultRoot: the
    /// folder shown while the state names none (UTF-8).
    void SetState(State* state, std::string defaultRoot);
    void Render();

  private:
    struct Entry
    {
      std::string name; // UTF-8 file or folder name
      std::string path; // UTF-8 full path
      bool isDirectory;
    };

    void RenderFilesTab();
    void RenderRecentTab();
    void RenderDirectory(const std::string& dir);
    const std::vector<Entry>& List(const std::string& dir);
    void SetRoot(std::string root);
    void TryLoad(const std::string& path);

    State* m_state = nullptr;
    LoadCallback m_onLoad;
    std::string m_selectedPath;
    std::string m_root;
    // Folder listings, read once per folder until Refresh or a new root: listing every
    // open folder on every frame stalls on slow or network drives.
    std::unordered_map<std::string, std::vector<Entry>> m_listings;
  };
} // namespace apui::samples::player
