// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "State.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace apui::samples::player
{
  namespace
  {
    std::string JsonEscape(const std::string& s)
    {
      std::string out;
      out.reserve(s.size() + 4);
      for (char c : s)
      {
        if (c == '"')
          out += "\\\"";
        else if (c == '\\')
          out += "\\\\";
        else if (c == '\n')
          out += "\\n";
        else if (c == '\r')
          out += "\\r";
        else
          out += c;
      }
      return out;
    }

    /// The string literal at or after pos; pos ends just past its closing quote.
    std::string ExtractString(const std::string& s, size_t& pos)
    {
      while (pos < s.size() && s[pos] != '"')
        ++pos;
      if (pos >= s.size())
        return { };
      ++pos;
      std::string out;
      while (pos < s.size())
      {
        if (s[pos] == '\\' && pos + 1 < s.size())
        {
          const char n = s[++pos];
          if (n == '"')
            out += '"';
          else if (n == '\\')
            out += '\\';
          else if (n == 'n')
            out += '\n';
          else if (n == 'r')
            out += '\r';
          ++pos;
        }
        else if (s[pos] == '"')
        {
          ++pos;
          break;
        }
        else
        {
          out += s[pos++];
        }
      }
      return out;
    }
  } // namespace

  State State::Load(const std::string& path)
  {
    State state;
    std::ifstream f(std::filesystem::u8path(path));
    if (!f.is_open())
      return state;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();

    auto readKey = [&](const char* key, std::string& out)
    {
      size_t p = j.find(key);
      if (p == std::string::npos)
        return;
      p += std::char_traits<char>::length(key);
      out = ExtractString(j, p);
    };
    readKey("\"workspaceRoot\"", state.workspaceRoot);
    readKey("\"lastUrl\"", state.lastUrl);

    size_t p = j.find("\"recentFiles\"");
    if (p != std::string::npos)
    {
      p = j.find('[', p);
      if (p != std::string::npos)
      {
        ++p;
        while (p < j.size() && j[p] != ']' && state.recentFiles.size() < kMaxRecent)
        {
          if (j[p] == '"')
            state.recentFiles.push_back(ExtractString(j, p));
          else
            ++p;
        }
      }
    }
    return state;
  }

  void State::Save(const std::string& path) const
  {
    std::ofstream f(std::filesystem::u8path(path));
    if (!f.is_open())
      return;
    f << "{\n"
      << "  \"workspaceRoot\": \"" << JsonEscape(workspaceRoot) << "\",\n"
      << "  \"lastUrl\": \"" << JsonEscape(lastUrl) << "\",\n"
      << "  \"recentFiles\": [\n";
    for (size_t i = 0; i < recentFiles.size(); ++i)
    {
      f << "    \"" << JsonEscape(recentFiles[i]) << "\"";
      if (i + 1 < recentFiles.size())
        f << ',';
      f << '\n';
    }
    f << "  ]\n}\n";
  }

  void State::AddRecent(const std::string& url)
  {
    const auto it = std::find(recentFiles.begin(), recentFiles.end(), url);
    if (it != recentFiles.end())
      recentFiles.erase(it);
    recentFiles.push_front(url);
    if (recentFiles.size() > kMaxRecent)
      recentFiles.pop_back();
  }
} // namespace apui::samples::player
