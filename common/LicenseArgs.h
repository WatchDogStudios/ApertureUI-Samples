// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

namespace apui::samples
{
  /// Licence settings for a sample: command-line arguments win over environment
  /// variables.
  ///
  /// Each flag takes its value as "--flag=value" or as the next argument. A next argument
  /// that starts with '-' is the next option, never a value (give such a value with '='),
  /// and a flag is never allowed to swallow another one. A flag with no value or an empty
  /// one is recorded in `problems` and leaves the environment value in place.
  struct LicenseArgs
  {
    std::string key;             // --license-key / APUI_LICENSE_KEY
    std::string projectId;       // --project-id / APUI_PROJECT_ID
    std::string certificatePath; // --certificate / APUI_CERTIFICATE
    /// argv[0] plus every argument that is not a licence flag
    std::vector<std::string> rest;
    /// One line per licence flag that had no usable value, for the caller to report.
    /// Each names the flag only, never a value: these are keys.
    std::vector<std::string> problems;
  };

  LicenseArgs ParseLicenseArgs(int argc, char** argv);

  /// Starts the SDK with these settings. Returns false and fills `error` when the SDK is
  /// not active.
  bool StartSdk(const LicenseArgs& args, std::string& error);
} // namespace apui::samples
