// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "MinimalHost.h"

int main(int argc, char** argv)
{
  apui::samples::MinimalHost host;
  if (!host.Initialize("Aperture UI: Data Binding", "Data/data-binding/index.html", argc, argv))
    return 1;

  host.View().setDataModel("player",
    R"({"name":"Ada","health":72,"level":7,"alive":true})");
  int health = 72;
  float elapsed = 0.0f;
  return host.Run([&](float deltaSeconds) {
    elapsed += deltaSeconds;
    if (elapsed >= 1.0f)
    {
      elapsed -= 1.0f;
      health = health > 15 ? health - 7 : 72;
      const std::string model = "{\"name\":\"Ada\",\"health\":"
        + std::to_string(health) + ",\"level\":7,\"alive\":true}";
      host.View().setDataModel("player", model);
    }
  });
}