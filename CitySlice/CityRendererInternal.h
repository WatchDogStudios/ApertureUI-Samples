/*
 * CityRendererInternal.h - helpers shared by CityRenderer.cpp (passes) and
 * CityModels.cpp (procedural meshes): packed colours, hashing, day/night light.
 */
#pragma once

#include "City.h"
#include "CityMath.h"
#include "CityRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

constexpr float kPi = 3.14159265f;
constexpr float kCell = City::kCell;

/// Packed vertex colour: R in the low byte, material id in the alpha byte.
constexpr uint32_t Col(uint32_t r, uint32_t g, uint32_t b, uint32_t mat = CityRenderer::MatPlain)
{
  return (mat << 24) | (b << 16) | (g << 8) | r;
}

inline uint32_t Hash8(int x, int y, int salt)
{
  uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u
    ^ static_cast<uint32_t>(salt) * 83492791u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h & 0xFF;
}

/// Shift a packed colour by a signed delta on every channel (material kept).
inline uint32_t Shade(uint32_t c, int delta)
{
  auto ch = [&](int shift) {
    int v = static_cast<int>((c >> shift) & 0xFF) + delta;
    return static_cast<uint32_t>(std::clamp(v, 0, 255)) << shift;
  };
  return (c & 0xFF000000u) | ch(0) | ch(8) | ch(16);
}

inline uint32_t WithMat(uint32_t c, uint32_t mat) { return (c & 0x00FFFFFFu) | (mat << 24); }

inline uint32_t ColF(Vec3 c, uint32_t mat)
{
  auto q = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return Col(q(c.x), q(c.y), q(c.z), mat);
}

inline Vec3 ColVec(uint32_t c)
{
  return { (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f };
}

inline float Smooth01(float t)
{
  t = Clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/// Sun/moon, tint, ambient, night factor, horizon and zenith colours for a
/// time of day. phase 0 = midnight, 0.25 = sunrise, 0.5 = noon.
struct DayLight
{
  Vec3 sunDir, sunColor, horizon, zenith;
  float ambient, night, sunVisible;
};

inline DayLight ComputeDayLight(double day)
{
  const float phase = static_cast<float>(day - std::floor(day));
  const float ang = (phase - 0.25f) * 2.0f * kPi; // 0 at sunrise
  const float elev = std::sin(ang);
  const float night = Clamp(1.0f - (elev + 0.08f) / 0.28f, 0.0f, 1.0f);
  const float dusk = 1.0f - Clamp(std::fabs(elev) / 0.3f, 0.0f, 1.0f);

  DayLight L;
  // Below the horizon the "sun" becomes a dim moon from the opposite side.
  const float e = elev >= 0.0f ? std::max(elev, 0.08f) : std::max(-elev, 0.25f);
  const float az = elev >= 0.0f ? std::cos(ang) : -std::cos(ang);
  L.sunDir = Vec3 { az * 0.75f, e, 0.45f }.Normalized();
  const Vec3 noon { 1.0f, 0.96f, 0.88f }, duskC { 1.0f, 0.58f, 0.32f }, moon { 0.28f, 0.34f, 0.52f };
  Vec3 sunC = noon * (1.0f - dusk) + duskC * dusk;
  L.sunColor = sunC * (1.0f - night) + moon * 0.55f * night;
  L.ambient = Lerp(0.55f, 0.2f, night);
  L.night = night;
  L.sunVisible = Clamp(elev * 5.0f, 0.0f, 1.0f);
  const Vec3 dayH { 0.36f, 0.52f, 0.76f }, duskH { 0.78f, 0.45f, 0.3f }, nightH { 0.02f, 0.03f, 0.07f };
  const Vec3 dayZ { 0.16f, 0.36f, 0.78f }, duskZ { 0.24f, 0.2f, 0.46f }, nightZ { 0.008f, 0.012f, 0.035f };
  Vec3 h = dayH * (1.0f - dusk) + duskH * dusk;
  Vec3 z = dayZ * (1.0f - dusk) + duskZ * dusk;
  L.horizon = h * (1.0f - night) + nightH * night;
  L.zenith = z * (1.0f - night) + nightZ * night;
  return L;
}
