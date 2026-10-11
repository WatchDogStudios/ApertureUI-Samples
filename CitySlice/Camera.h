/*
 * Camera.h - RTS orbit camera for CitySlice (target on the ground, yaw/pitch/distance).
 *
 * Input writes the goal fields; Update() eases the rendered pose towards them
 * so panning, zooming and orbiting feel damped instead of stepping.
 */
#pragma once

#include "CityMath.h"

struct Camera
{
  // Goal pose (what input manipulates).
  Vec3 target { 384.0f, 0.0f, 384.0f };
  float yaw = 0.8f;    // radians around +Y
  float pitch = 0.95f; // radians above the ground plane
  float dist = 260.0f;
  float fovY = 0.85f;  // ~49 degrees
  float zNear = 1.0f;
  float zFar = 3000.0f;

  // Rendered pose (eased towards the goal).
  Vec3 sTarget { 384.0f, 0.0f, 384.0f };
  float sYaw = 0.8f, sPitch = 0.95f, sDist = 260.0f;

  void Update(float dt)
  {
    const float k = 1.0f - std::exp(-dt * 9.0f);
    sTarget += (target - sTarget) * k;
    sYaw += (yaw - sYaw) * k;
    sPitch += (pitch - sPitch) * k;
    sDist += (dist - sDist) * k;
  }

  void Snap()
  {
    sTarget = target;
    sYaw = yaw;
    sPitch = pitch;
    sDist = dist;
  }

  Vec3 Forward() const // unit vector from eye towards target (rendered pose)
  {
    return Vec3 { -std::cos(sPitch) * std::sin(sYaw), -std::sin(sPitch), -std::cos(sPitch) * std::cos(sYaw) };
  }
  Vec3 Eye() const { return sTarget - Forward() * sDist; }

  Mat4 View() const { return Mat4::LookAt(Eye(), sTarget, { 0.0f, 1.0f, 0.0f }); }
  Mat4 Proj(float aspect) const { return Mat4::Perspective(fovY, aspect, zNear, zFar); }

  /// World-space direction of the ray through pixel (px,py) of a w x h viewport.
  Vec3 RayDir(float px, float py, float w, float h) const
  {
    const Vec3 f = Forward();
    const Vec3 r = f.Cross({ 0.0f, 1.0f, 0.0f }).Normalized();
    const Vec3 u = r.Cross(f);
    const float t = std::tan(fovY * 0.5f);
    const float nx = (2.0f * px / w - 1.0f) * t * (w / h);
    const float ny = (1.0f - 2.0f * py / h) * t;
    return (f + r * nx + u * ny).Normalized();
  }

  void Orbit(float dxPixels, float dyPixels)
  {
    yaw -= dxPixels * 0.005f;
    pitch = Clamp(pitch + dyPixels * 0.005f, 0.25f, 1.45f);
  }

  /// Pan in the camera's ground-projected frame; dx/dz in world metres.
  void Pan(float dx, float dz)
  {
    const Vec3 f = Forward();
    const Vec3 fwd = Vec3 { f.x, 0.0f, f.z }.Normalized();
    const Vec3 right = fwd.Cross({ 0.0f, 1.0f, 0.0f });
    target += right * dx + fwd * dz;
  }

  void Zoom(float wheelSteps) { dist = Clamp(dist * std::pow(0.85f, wheelSteps), 25.0f, 900.0f); }
};
