// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
#include "../LicenseArgs.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using apui::samples::LicenseArgs;

static int g_failures = 0;
static void Expect(bool ok, const char* what)
{
  if (!ok)
  {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

/// argv[0] is added in front.
static LicenseArgs Parse(std::vector<std::string> args)
{
  args.insert(args.begin(), "app");
  std::vector<char*> argv;
  for (std::string& a : args)
    argv.push_back(a.data());
  return apui::samples::ParseLicenseArgs(static_cast<int>(argv.size()), argv.data());
}

static bool Rest(const LicenseArgs& l, std::vector<std::string> expected)
{
  expected.insert(expected.begin(), "app");
  return l.rest == expected;
}

static bool OnlyProblem(const LicenseArgs& l, const char* text)
{
  return l.problems.size() == 1 && l.problems[0] == text;
}

int main()
{
  const std::string kEnvKey = "ENV-KEY";
  const std::string kEnvProject = "11111111-1111-1111-1111-111111111111";
  _putenv_s("APUI_LICENSE_KEY", kEnvKey.c_str());
  _putenv_s("APUI_PROJECT_ID", kEnvProject.c_str());
  _putenv_s("APUI_CERTIFICATE", "");

  {
    char a0[] = "app", a1[] = "--license-key", a2[] = "  ARG-KEY  ", a3[] = "--html",
         a4[] = "page.html";
    char* argv[] = { a0, a1, a2, a3, a4 };
    LicenseArgs l = apui::samples::ParseLicenseArgs(5, argv);
    Expect(l.key == "ARG-KEY", "argument wins over environment and is trimmed");
    Expect(l.projectId == kEnvProject, "project id falls back to environment");
    Expect(l.certificatePath.empty(), "empty environment value means unset");
    Expect(l.rest.size() == 3 && l.rest[1] == "--html" && l.rest[2] == "page.html",
      "other arguments are kept in order");
    Expect(l.problems.empty(), "a good command line has no problems");
  }
  {
    const LicenseArgs l = Parse({ "--certificate" });
    Expect(l.certificatePath.empty() && Rest(l, { }),
      "a flag with no value is dropped, not read past the end");
    Expect(OnlyProblem(l, "--certificate has no value"),
      "a flag at the end of the line is a problem");
  }

  // A flag must never take the next flag for its value.
  {
    const LicenseArgs l = Parse({ "--certificate", "--devtools-token", "T" });
    Expect(l.certificatePath.empty(), "the next option is not the certificate path");
    Expect(Rest(l, { "--devtools-token", "T" }),
      "the option after a valueless flag is left for the caller");
    Expect(
      OnlyProblem(l, "--certificate has no value"), "the valueless flag is reported");
  }
  {
    const LicenseArgs l = Parse({ "--license-key", "--project-id", "P" });
    Expect(l.key == kEnvKey, "the key keeps its environment value");
    Expect(l.projectId == "P", "the next flag is still read as itself");
    Expect(OnlyProblem(l, "--license-key has no value"), "only the key is reported");
  }
  {
    const LicenseArgs l = Parse({ "--project-id", "-watch", "--single-frame" });
    Expect(l.projectId == kEnvProject && Rest(l, { "-watch", "--single-frame" }),
      "a single-dash option is not a value either");
    Expect(OnlyProblem(l, "--project-id has no value"), "and it is reported");
  }
  {
    const LicenseArgs l = Parse({ "--license-key=-LEADING-DASH" });
    Expect(l.key == "-LEADING-DASH" && l.problems.empty(),
      "a value that starts with '-' is given with '='");
  }
  {
    const LicenseArgs l = Parse({ "--license-key", "-LEADING-DASH" });
    Expect(l.key == kEnvKey && Rest(l, { "-LEADING-DASH" }) && l.problems.size() == 1,
      "the separate form never takes a value that starts with '-'");
  }

  // The '=' forms.
  {
    const LicenseArgs l = Parse(
      { "--license-key=K1", "--project-id=P1", "--certificate=cert.license", "--watch" });
    Expect(l.key == "K1" && l.projectId == "P1" && l.certificatePath == "cert.license",
      "each flag takes '=value'");
    Expect(
      Rest(l, { "--watch" }) && l.problems.empty(), "'=' flags are not left in rest");
  }
  {
    const LicenseArgs l = Parse({ "--license-key=a=b", "--project-id=  P2  " });
    Expect(l.key == "a=b", "only the first '=' separates the value");
    Expect(l.projectId == "P2", "an '=' value is trimmed");
  }
  {
    const LicenseArgs l = Parse({ "--license-key=" });
    Expect(l.key == kEnvKey, "an empty '=' value leaves the environment value");
    Expect(OnlyProblem(l, "--license-key is empty"), "an empty '=' value is reported");
  }
  {
    const LicenseArgs l = Parse({ "--project-id=   ", "--certificate", "" });
    Expect(l.projectId == kEnvProject && l.certificatePath.empty(),
      "a blank value is no value");
    Expect(l.problems.size() == 2 && l.problems[0] == "--project-id is empty"
        && l.problems[1] == "--certificate is empty",
      "blank values are reported, in order");
  }
  {
    const LicenseArgs l = Parse(
      { "--license-keys=X", "--license-key-x", "--certificates", "--project-id=P3" });
    Expect(Rest(l, { "--license-keys=X", "--license-key-x", "--certificates" })
        && l.key == kEnvKey && l.projectId == "P3" && l.problems.empty(),
      "a longer option name is not a licence flag");
  }
  // The flag names match in any letter case, in both forms.
  {
    const LicenseArgs l = Parse({ "--License-Key", "K2", "--PROJECT-ID=P4",
      "--cErTiFiCaTe", "cert2.license", "--watch" });
    Expect(l.key == "K2" && l.projectId == "P4" && l.certificatePath == "cert2.license",
      "a flag name in another letter case is still the flag");
    Expect(Rest(l, { "--watch" }) && l.problems.empty(),
      "no case variant is left in rest, where it would be taken for a document path");
  }
  {
    // A problem names the canonical flag, never the spelling or the value given.
    const LicenseArgs l = Parse({ "--LICENSE-KEY", "--Project-Id=", "--CERTIFICATE=  " });
    Expect(l.problems.size() == 3 && l.problems[0] == "--license-key has no value"
        && l.problems[1] == "--project-id is empty"
        && l.problems[2] == "--certificate is empty",
      "a case variant is reported under the flag's own name");
    Expect(Rest(l, { }), "and is not passed on");
  }
  {
    // A longer name in another case is still not a licence flag.
    const LicenseArgs l = Parse({ "--LICENSE-KEYS=X", "--Certificates" });
    Expect(Rest(l, { "--LICENSE-KEYS=X", "--Certificates" }) && l.problems.empty()
        && l.key == kEnvKey,
      "matching ignores letter case, not the length of the name");
  }
  {
    // A problem names the flag and nothing else, so a key never reaches a log through it.
    const LicenseArgs l = Parse({ "--license-key", "--certificate=SECRET-VALUE",
      "--project-id=", "--certificate" });
    bool leaked = false;
    for (const std::string& p : l.problems)
      leaked = leaked || p.find("SECRET-VALUE") != std::string::npos;
    Expect(l.certificatePath == "SECRET-VALUE", "the later flag still sets its value");
    Expect(!leaked && l.problems.size() == 3, "problems carry no values");
  }

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
