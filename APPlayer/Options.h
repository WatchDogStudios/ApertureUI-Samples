// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// Options.h - APPlayer's command line, apart from the licence flags, which
// common/LicenseArgs.h takes out first. The options are listed in App.cpp.

#pragma once

#include <string>
#include <vector>

namespace apui::samples::player
{
  struct Options
  {
    bool singleFrame = false;
    unsigned captureAfterFrames = 0;
    std::string capturePath; // UTF-8; empty for the default in %TEMP%
    bool vulkan = false;
    bool watch = false;
    bool gpuFont = false;
    std::string documentPath;              // UTF-8 absolute path
    std::string documentUrl;               // http(s)
    std::vector<std::string> contentRoots; // UTF-8 absolute paths
    bool devToolsTokenGiven = false;
    std::string devToolsToken;
    std::string devToolsTokenProblem;  // why the given token is unusable (DevTools off)
    std::vector<std::string> warnings; // logged once the log is up
  };

  /// args: the UTF-8 command line, argv[0] first (LicenseArgs::rest).
  Options ParseOptions(const std::vector<std::string>& args);

  /// utf8 as an absolute, normalised UTF-8 path with forward slashes.
  std::string AbsolutePath(const std::string& utf8);

  bool IsHttpUrl(const std::string& s);
} // namespace apui::samples::player
