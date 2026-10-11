// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "MinimalHost.h"

int main(int argc, char** argv)
{
  apui::samples::MinimalHost host;
  if (!host.Initialize("Aperture UI: Hello View", "Data/hello-view/index.html", argc, argv))
    return 1;
  return host.Run();
}