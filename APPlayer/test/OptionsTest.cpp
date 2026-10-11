// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// APPlayer's command-line parser, above all the DevTools token rules: given at all,
// --devtools-token yields a usable token or a reason DevTools stays off, never "no
// token".
#include "../Options.h"

#include <cstdio>
#include <string>
#include <vector>

using apui::samples::player::Options;
using apui::samples::player::ParseOptions;

static int g_failures = 0;
static void Expect(bool ok, const char* what)
{
  if (!ok)
  {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

static Options Parse(std::vector<std::string> args)
{
  args.insert(args.begin(), "apertureplayer");
  return ParseOptions(args);
}

static bool Refused(const Options& o, const char* problem)
{
  return o.devToolsTokenGiven && o.devToolsToken.empty()
    && o.devToolsTokenProblem == problem;
}

int main()
{
  // Low-entropy tokens, built here, so secret scanners have nothing to match.
  const std::string token(20, 'a');
  const std::string dashToken = "-" + std::string(19, 'b');
  const std::string kEq = std::string("--devtools-token") + "=";

  {
    const Options o = Parse({ kEq + token });
    Expect(
      o.devToolsTokenGiven && o.devToolsToken == token && o.devToolsTokenProblem.empty(),
      "the '=' form gives the token");
  }
  {
    const Options o = Parse({ "--devtools-token", token, "--watch" });
    Expect(
      o.devToolsToken == token && o.watch, "the separate form takes the next argument");
  }
  {
    const Options o = Parse({ kEq + dashToken });
    Expect(
      o.devToolsToken == dashToken, "a token that starts with '-' is given with '='");
  }
  {
    const Options o = Parse({ "--devtools-token", "-watch" });
    Expect(Refused(o, "has no value") && o.watch,
      "a following single-dash option is not the value, and still counts");
  }
  {
    const Options o = Parse({ "--devtools-token", "--single-frame" });
    Expect(
      Refused(o, "has no value") && o.singleFrame, "a following option is not the value");
  }
  Expect(
    Refused(Parse({ "--devtools-token" }), "has no value"), "a missing value is refused");
  Expect(Refused(Parse({ kEq }), "is empty"), "an empty '=' value is refused");
  Expect(
    Refused(Parse({ "--devtools-token", "" }), "is empty"), "an empty value is refused");
  Expect(Refused(Parse({ kEq + "\xC5\x81" + token }), "has a character outside ASCII"),
    "a character outside ASCII (U+0141, as UTF-8) is refused");
  Expect(Refused(Parse({ kEq + std::string(15, 'a') }), "is shorter than 16 characters"),
    "a short token is refused");
  Expect(Refused(Parse({ kEq + token + "/x" }),
           "has a character other than A-Z, a-z, 0-9, '_' or '-'"),
    "a character outside the URL-safe set is refused");
  Expect(Parse({ kEq + std::string(16, 'Z') + "_-09" }).devToolsTokenProblem.empty(),
    "letters, digits, '_' and '-' are accepted");
  {
    const Options o = Parse({ kEq + token, kEq });
    Expect(Refused(o, "is empty"),
      "a later unusable token is not replaced by the earlier one");
  }
  {
    const Options o = Parse({ "--html" });
    Expect(!o.devToolsTokenGiven && o.devToolsToken.empty(), "no flag, no token");
  }

  {
    const Options o = Parse({ "--capture-after=30" });
    Expect(o.captureAfterFrames == 30 && o.capturePath.empty(),
      "capture frames, default file");
  }
  {
    const Options o = Parse({ "--capture-after=5=shot.bmp" });
    Expect(o.captureAfterFrames == 5 && o.capturePath.size() > 9
        && o.capturePath.compare(o.capturePath.size() - 9, 9, "/shot.bmp") == 0,
      "capture frames and a file, made absolute");
  }
  {
    const Options o = Parse({ "--capture-after=0", "--capture-after=x" });
    Expect(o.captureAfterFrames == 0 && o.warnings.size() == 2,
      "bad frame counts are reported");
  }
  {
    const Options o = Parse({ "http://localhost:5173/", "--VULKAN", "--bogus" });
    Expect(o.documentUrl == "http://localhost:5173/" && o.documentPath.empty(),
      "a bare http(s) argument is a URL");
    Expect(o.vulkan, "options are not case-sensitive");
    Expect(o.warnings.size() == 1 && o.warnings[0].find("--bogus") != std::string::npos,
      "an unknown option is reported");
  }
  {
    const Options o = Parse({ "--html", "page.html", "--content-root=assets" });
    Expect(o.documentPath.size() > 10
        && o.documentPath.compare(o.documentPath.size() - 10, 10, "/page.html") == 0
        && o.documentPath.find('\\') == std::string::npos,
      "a file is made absolute, with forward slashes");
    Expect(o.contentRoots.size() == 1, "content roots are collected");
  }

  // The length rule's edges: 16 is the shortest accepted token, 15 the longest refused.
  {
    const Options o = Parse({ kEq + std::string(16, 'a') });
    Expect(o.devToolsToken == std::string(16, 'a') && o.devToolsTokenProblem.empty(),
      "a token of exactly 16 characters is accepted");
  }
  {
    const Options o = Parse({ kEq + std::string(15, 'a') });
    Expect(Refused(o, "is shorter than 16 characters"),
      "a token of 15 characters is refused, and nothing is kept");
  }
  {
    const Options o = Parse({ kEq + "short", kEq + token });
    Expect(
      o.devToolsTokenGiven && o.devToolsToken == token && o.devToolsTokenProblem.empty(),
      "a good token after a bad one clears the problem");
  }
  {
    const Options o = Parse({ kEq + token, kEq + "short" });
    Expect(Refused(o, "is shorter than 16 characters"),
      "a bad token after a good one is refused, whatever came before");
  }

  // Value options with no usable value are reported, never dropped silently.
  {
    const Options o = Parse({ "--url" });
    Expect(o.warnings.size() == 1 && o.warnings[0].find("--url has no value") == 0
        && o.documentUrl.empty(),
      "--url as the last argument is reported");
  }
  {
    const Options o = Parse({ "--html" });
    Expect(o.warnings.size() == 1 && o.warnings[0].find("--html has no value") == 0
        && o.documentPath.empty() && o.documentUrl.empty(),
      "--html as the last argument is reported");
  }
  {
    const Options o = Parse({ "--html", "--watch", "--url", "-single-frame" });
    Expect(o.warnings.size() == 2 && o.watch && o.singleFrame && o.documentPath.empty()
        && o.documentUrl.empty(),
      "a following option is not taken for the document, and still counts");
  }
  {
    const Options o = Parse({ "--url=", "--html=", "--content-root=" });
    Expect(o.warnings.size() == 3 && o.warnings[0].find("--url is empty") == 0
        && o.warnings[1].find("--html is empty") == 0
        && o.warnings[2].find("--content-root is empty") == 0 && o.contentRoots.empty(),
      "an empty '=' value is reported for --url, --html and --content-root");
  }
  {
    const Options o = Parse({ "--url", "http://localhost:5173/", "-html=page.html" });
    Expect(o.warnings.empty() && o.documentUrl == "http://localhost:5173/"
        && !o.documentPath.empty(),
      "good values are not reported");
  }

  // An unknown option is logged by name only: a mistyped licence flag carries a key.
  {
    const Options o
      = Parse({ "--license-kye=SECRET-VALUE", "--bogus", "--x=a=b", "-q=" });
    Expect(o.warnings.size() == 4
        && o.warnings[0] == "ignoring unknown option --license-kye"
        && o.warnings[1] == "ignoring unknown option --bogus"
        && o.warnings[2] == "ignoring unknown option --x"
        && o.warnings[3] == "ignoring unknown option -q",
      "the warning names the option and stops at '='");
    bool leaked = false;
    for (const std::string& w : o.warnings)
      leaked = leaked || w.find("SECRET-VALUE") != std::string::npos;
    Expect(!leaked, "no warning contains a value");
  }

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
