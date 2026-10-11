// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
#include "LicenseArgs.h"

#include <cstdlib>
#include <cstring>
#include <string.h>

namespace apui::samples
{
  namespace
  {
    std::string Trim(const std::string& s)
    {
      const size_t b = s.find_first_not_of(" \t\r\n");
      if (b == std::string::npos)
        return { };
      const size_t e = s.find_last_not_of(" \t\r\n");
      return s.substr(b, e - b + 1);
    }

    std::string Env(const char* name)
    {
      char* value = nullptr;
      size_t len = 0;
      std::string out;
      if (_dupenv_s(&value, &len, name) == 0 && value != nullptr)
        out = Trim(value);
      std::free(value);
      return out;
    }
  } // namespace

  LicenseArgs ParseLicenseArgs(int argc, char** argv)
  {
    struct Flag
    {
      const char* name;
      std::string LicenseArgs::* field;
    };
    static const Flag kFlags[] = {
      { "--license-key", &LicenseArgs::key },
      { "--project-id", &LicenseArgs::projectId },
      { "--certificate", &LicenseArgs::certificatePath },
    };

    LicenseArgs out;
    out.key = Env("APUI_LICENSE_KEY");
    out.projectId = Env("APUI_PROJECT_ID");
    out.certificatePath = Env("APUI_CERTIFICATE");
    for (int i = 0; i < argc; ++i)
    {
      const std::string a = argv[i];
      const Flag* flag = nullptr;
      const char* value = nullptr; // null: the flag has no value
      for (const Flag& f : kFlags)
      {
        // The names match in any letter case, as the player's own options do: a flag
        // typed as --License-Key must not fall through to the document path, and be
        // logged with its key.
        const size_t n = std::strlen(f.name);
        if (_stricmp(a.c_str(), f.name) == 0)
        {
          flag = &f;
          // The next argument is the value unless it is another option.
          if (i + 1 < argc && argv[i + 1][0] != '-')
            value = argv[++i];
        }
        else if (a.size() > n && _strnicmp(a.c_str(), f.name, n) == 0 && a[n] == '=')
        {
          flag = &f;
          value = a.c_str() + n + 1;
        }
        if (flag != nullptr)
          break;
      }
      if (flag == nullptr)
      {
        out.rest.push_back(a);
        continue;
      }
      const std::string trimmed = value != nullptr ? Trim(value) : std::string();
      if (trimmed.empty())
        out.problems.push_back(
          std::string(flag->name) + (value != nullptr ? " is empty" : " has no value"));
      else
        out.*(flag->field) = trimmed;
    }
    return out;
  }
} // namespace apui::samples
