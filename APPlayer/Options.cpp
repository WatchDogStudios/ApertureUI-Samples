// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "Options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string.h>

namespace fs = std::filesystem;

namespace apui::samples::player
{
  namespace
  {
    bool Is(const std::string& arg, const char* a, const char* b = nullptr)
    {
      return _stricmp(arg.c_str(), a) == 0 || (b && _stricmp(arg.c_str(), b) == 0);
    }

    /// The value of an "<option>=<value>" argument, else nullptr. Not case-sensitive.
    const char* ValueOf(const std::string& arg, const char* option)
    {
      const size_t n = std::strlen(option);
      return _strnicmp(arg.c_str(), option, n) == 0 && arg.c_str()[n] == '='
        ? arg.c_str() + n + 1
        : nullptr;
    }
  } // namespace

  std::string AbsolutePath(const std::string& utf8)
  {
    std::error_code ec;
    fs::path p = fs::absolute(fs::u8path(utf8), ec);
    if (ec)
      p = fs::u8path(utf8);
    return p.lexically_normal().generic_u8string();
  }

  bool IsHttpUrl(const std::string& s)
  {
    return _strnicmp(s.c_str(), "http://", 7) == 0
      || _strnicmp(s.c_str(), "https://", 8) == 0;
  }

  Options ParseOptions(const std::vector<std::string>& args)
  {
    Options o;
    auto setDocument = [&o](const std::string& value)
    {
      if (value.empty())
        return;
      if (IsHttpUrl(value))
        o.documentUrl = value;
      else
        o.documentPath = AbsolutePath(value);
    };

    // The value of "--option <value>": the next argument, unless that is another option
    // (single-dash options such as -watch exist, so a value that starts with '-' is given
    // with '='). Null when there is none; the next argument is then left alone.
    auto takeNext = [&args](size_t& i) -> const char*
    {
      return i + 1 < args.size() && args[i + 1].rfind('-', 0) != 0 ? args[++i].c_str()
                                                                   : nullptr;
    };
    // True and a warning when a value-taking option has no usable value.
    auto missing = [&o](const char* option, const char* value, const char* usage)
    {
      if (value != nullptr && *value != '\0')
        return false;
      o.warnings.push_back(std::string(option) + (value ? " is empty" : " has no value")
        + " (" + usage + "); it is ignored");
      return true;
    };

    for (size_t i = 1; i < args.size(); ++i)
    {
      const std::string& a = args[i];
      const char* v = nullptr;
      if (Is(a, "--single-frame", "-single-frame"))
      {
        o.singleFrame = true;
      }
      else if (Is(a, "--vulkan", "-vulkan"))
      {
        o.vulkan = true;
      }
      else if (Is(a, "--d3d11", "-d3d11"))
      {
        o.vulkan = false;
      }
      else if (Is(a, "--watch", "-watch"))
      {
        o.watch = true;
      }
      else if (Is(a, "--gpu-font", "-gpu-font"))
      {
        o.gpuFont = true;
      }
      else if ((v = ValueOf(a, "--content-root")) != nullptr)
      {
        if (!missing("--content-root", v, "--content-root=<folder>"))
          o.contentRoots.push_back(AbsolutePath(v));
      }
      else if ((v = ValueOf(a, "--capture-after")) != nullptr)
      {
        // <frames>[=<file>]
        const char* eq = std::strchr(v, '=');
        const std::string frames = eq ? std::string(v, eq) : std::string(v);
        char* end = nullptr;
        const unsigned long n = std::strtoul(frames.c_str(), &end, 10);
        if (frames.empty() || *end != '\0' || n == 0 || n > 1000000)
          o.warnings.push_back(
            "--capture-after needs a frame count from 1 to 1000000: " + a);
        else
          o.captureAfterFrames = static_cast<unsigned>(n);
        if (eq && eq[1] != '\0')
          o.capturePath = AbsolutePath(eq + 1);
      }
      else if ((v = ValueOf(a, "--url")) != nullptr || Is(a, "--url", "-url"))
      {
        if (v == nullptr)
          v = takeNext(i);
        if (!missing("--url", v, "--url <url> or --url=<url>"))
          o.documentUrl = v;
      }
      else if ((v = ValueOf(a, "--html")) != nullptr
        || (v = ValueOf(a, "-html")) != nullptr || Is(a, "--html", "-html"))
      {
        if (v == nullptr)
          v = takeNext(i);
        if (!missing("--html", v, "--html <path> or --html=<path>"))
          setDocument(v);
      }
      else if ((v = ValueOf(a, "--devtools-token")) != nullptr
        || Is(a, "--devtools-token"))
      {
        // Given at all, the flag must yield a usable token; otherwise DevTools does not
        // start. It never falls back to running without one.
        o.devToolsTokenGiven = true;
        o.devToolsToken.clear();
        o.devToolsTokenProblem.clear();
        if (v == nullptr)
          v = takeNext(i);
        if (v == nullptr)
          o.devToolsTokenProblem = "has no value";
        else if (*v == '\0')
          o.devToolsTokenProblem = "is empty";
        // UTF-8: every byte of a character outside ASCII is above 0x7F.
        else if (std::any_of(v, v + std::strlen(v),
                   [](char c) { return static_cast<unsigned char>(c) > 0x7F; }))
          o.devToolsTokenProblem = "has a character outside ASCII";
        // The SDK writes the token raw into URLs and HTML. Checked here as well,
        // because not every SDK build refuses a weak or unsafe token itself.
        else if (std::strlen(v) < 16)
          o.devToolsTokenProblem = "is shorter than 16 characters";
        else if (!std::all_of(v, v + std::strlen(v),
                   [](char c)
                   {
                     return std::isalnum(static_cast<unsigned char>(c)) || c == '_'
                       || c == '-';
                   }))
          o.devToolsTokenProblem = "has a character other than A-Z, a-z, 0-9, '_' or '-'";
        else
          o.devToolsToken = v;
      }
      else if (!a.empty() && a[0] != '-')
      {
        setDocument(a); // a bare document: a URL or a file path
      }
      else
      {
        // The name only: a mistyped option may have been given a secret
        // ("--license-kye=...").
        o.warnings.push_back("ignoring unknown option " + a.substr(0, a.find('=')));
      }
    }
    return o;
  }
} // namespace apui::samples::player
