/*
 * CityMath.h - minimal vector/matrix math for CitySlice
 *
 * Self-contained on purpose: the renderer feeds the projection matrix to the
 * shader as four explicit columns, so there is no row/column-major ambiguity
 * to inherit from anyone else's conventions. Right-handed, Y up, Vulkan clip
 * space (depth 0..1, Y down) baked into Perspective().
 */
#pragma once

#include <cmath>

struct Vec3
{
  float x = 0, y = 0, z = 0;

  Vec3 operator+(Vec3 o) const { return { x + o.x, y + o.y, z + o.z }; }
  Vec3 operator-(Vec3 o) const { return { x - o.x, y - o.y, z - o.z }; }
  Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
  Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }

  float Dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 Cross(Vec3 o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
  float Length() const { return std::sqrt(Dot(*this)); }
  Vec3 Normalized() const
  {
    const float l = Length();
    return l > 1e-6f ? *this * (1.0f / l) : Vec3 {};
  }
};

/// Column-major 4x4: m[col * 4 + row]. Columns are what the shader consumes.
struct Mat4
{
  float m[16] = {};

  static Mat4 Identity()
  {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }

  /// Vulkan-ready projection: depth maps to [0,1], clip Y is flipped so that
  /// world +Y is up on screen without a viewport trick.
  static Mat4 Perspective(float fovY, float aspect, float zNear, float zFar)
  {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r;
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = (zNear * zFar) / (zNear - zFar);
    return r;
  }

  static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up)
  {
    const Vec3 f = (target - eye).Normalized();
    const Vec3 s = f.Cross(up).Normalized();
    const Vec3 u = s.Cross(f);
    Mat4 r = Identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -s.Dot(eye);
    r.m[13] = -u.Dot(eye);
    r.m[14] = f.Dot(eye);
    return r;
  }

  /// General inverse (cofactors); identity when singular.
  Mat4 Inverse() const
  {
    const float* a = m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-12f)
      return Identity();
    Mat4 r;
    for (int i = 0; i < 16; ++i)
      r.m[i] = inv[i] / det;
    return r;
  }

  Mat4 operator*(const Mat4& b) const
  {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row)
      {
        float acc = 0.0f;
        for (int k = 0; k < 4; ++k)
          acc += m[k * 4 + row] * b.m[c * 4 + k];
        r.m[c * 4 + row] = acc;
      }
    return r;
  }
};

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
