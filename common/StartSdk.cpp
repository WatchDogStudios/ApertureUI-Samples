// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// StartSdk lives apart from the parser so that the parser's test needs no SDK.
#include "LicenseArgs.h"

#include <APHTML/APEngine.h>
#include <APHTML/API/LicenseConfig.h>

namespace apui::samples
{
  bool StartSdk(const LicenseArgs& args, std::string& error)
  {
    aperture::LicenseConfig cfg;
    cfg.key = args.key;
    cfg.projectId = args.projectId;
    cfg.certificatePath = args.certificatePath;
    aperture::ApertureSDK::Initialize(cfg);
    if (aperture::ApertureSDK::IsSDKActive())
      return true;
    error = "Aperture UI did not start: no valid licence. Pass --license-key <key> "
            "--project-id <uuid> (development builds) or --certificate <path to "
            "aperture.license> (shipping builds), or set APUI_LICENSE_KEY / "
            "APUI_PROJECT_ID / APUI_CERTIFICATE.";
    return false;
  }
} // namespace apui::samples
