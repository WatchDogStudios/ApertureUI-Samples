// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// State.h - what APPlayer remembers between runs: the workspace folder, the last
// document and the recently opened documents. Stored as a small JSON file.

#pragma once

#include <deque>
#include <string>

namespace apui::samples::player
{
  class State
  {
  public:
    static constexpr size_t kMaxRecent = 20;

    std::string workspaceRoot;           // UTF-8
    std::deque<std::string> recentFiles; // newest first, at most kMaxRecent
    std::string lastUrl;

    /// Reads the file State::Save wrote; a missing or unreadable file gives the defaults.
    /// Paths are UTF-8.
    static State Load(const std::string& path);
    void Save(const std::string& path) const;
    void AddRecent(const std::string& url);
  };
} // namespace apui::samples::player
