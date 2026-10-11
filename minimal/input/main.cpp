// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "MinimalHost.h"

int main(int argc, char** argv)
{
  apui::samples::MinimalHost host;
  if (!host.Initialize("Aperture UI: Input", "Data/input/index.html", argc, argv))
    return 1;
  return host.Run();
}