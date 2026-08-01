//
// Created by XF on 8/30/2024.
//

#include <cmath>
#include "Quaternion.h"
#include "Vector3.h"
Quaternion Quaternion::operator+(Quaternion other) const {
  return {x + other.x, y + other.y, z + other.z, w + other.w};
}
Quaternion Quaternion::operator-(Quaternion other) const {
  return {x - other.x, y - other.y, z - other.z, w - other.w};
}
Quaternion Quaternion::operator*(Quaternion other) const {
  float rX = x * other.w + w * other.x + y * other.z - z * other.y;
  float rY = y * other.w + w * other.y + z * other.x - x * other.z;
  float rZ = z * other.w + w * other.z + x * other.y - y * other.x;
  float rW = w * other.w - x * other.x - y * other.y - z * other.z;
  return {rX, rY, rZ, rW};
}
Quaternion Quaternion::operator*(float scalar) const {
  return {x * scalar, y * scalar, z * scalar, w * scalar};
}
Quaternion Quaternion::operator/(float scalar) const {
  return {x / scalar, y / scalar, z / scalar, w / scalar};
}
Quaternion &Quaternion::operator+=(Quaternion &other) {
  x += other.x;
  y += other.y;
  z += other.z;
  w += other.w;
  return *this;
}
Quaternion &Quaternion::operator-=(Quaternion &other) {
  x -= other.x;
  y -= other.y;
  z -= other.z;
  w -= other.w;
  return *this;
}
Quaternion &Quaternion::operator*=(Quaternion &other) {
  float rX = x * other.w + w * other.x + y * other.z - z * other.y;
  float rY = y * other.w + w * other.y + z * other.x - x * other.z;
  float rZ = z * other.w + w * other.z + x * other.y - y * other.x;
  float rW = w * other.w - x * other.x - y * other.y - z * other.z;
  x = rX;
  y = rY;
  z = rZ;
  w = rW;
  return *this;
}
Quaternion &Quaternion::operator*=(float scalar) {
  x *= scalar;
  y *= scalar;
  z *= scalar;
  w *= scalar;
  return *this;
}
Quaternion &Quaternion::operator/=(float scalar) {
  x /= scalar;
  y /= scalar;
  z /= scalar;
  w /= scalar;
  return *this;
}
bool Quaternion::operator==(Quaternion &other) const {
  return x == other.x && y == other.y && z == other.z && w == other.w;
}
bool Quaternion::operator!=(Quaternion &other) const {
  return x != other.x || y != other.y || z != other.z || w != other.w;
}
float Quaternion::dot(const Quaternion &other) const {
  return x * other.x + y * other.y + z * other.z + w * other.w;
}
float Quaternion::magnitude() const {
  return sqrtf(x * x + y * y + z * z + w * w);
}
Quaternion Quaternion::normalized() const {
  float mag = magnitude();
  return {x / mag, y / mag, z / mag, w / mag};
}
Quaternion Quaternion::conjugate() const { return {-x, -y, -z, w}; }
Quaternion Quaternion::inverse() const {
  return conjugate() / (x * x + y * y + z * z + w * w);
}
Quaternion Quaternion::lerp(const Quaternion &other, float t) const {
  return *this * (1 - t) + other * t;
}
Quaternion Quaternion::slerp(const Quaternion &other, float t) const {
  float dotProduct = dot(other);
  float theta = acosf(dotProduct);
  return (*this * sinf((1 - t) * theta) + other * sinf(t * theta)) /
         sinf(theta);
}
Vector3 Quaternion::rotate(const Vector3 &v) const {
  Quaternion conjugate = this->conjugate();
  Quaternion w = (*this * Quaternion(v.x, v.y, v.z, 0) * conjugate);
  return {w.x, w.y, w.z};
}
Vector3 Quaternion::getEulerAngles() const {
  return toEuler();
}
Vector3 Quaternion::getForward() const {
  return {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
}
Vector3 Quaternion::getUp() const {
  return {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
}
Vector3 Quaternion::getRight() const {
  return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
}
Vector3 Quaternion::getAxis() const { return {x, y, z}; }
float Quaternion::getAngle() const { return 2 * acosf(w); }
Vector3 Quaternion::getAxisAngle() const {

  float angle = getAngle();
  if (angle == 0) {
    return {0, 0, 0};
  }
  return {x / sinf(angle / 2), y / sinf(angle / 2), z / sinf(angle / 2)};
}
Quaternion Quaternion::operator*(Vector3 other) const {
  Quaternion q = {other.x, other.y, other.z, 0};
  Quaternion result = *this * q;
  return result;
}
Quaternion Quaternion::lookRotation(const Vector3 &forward, const Vector3 &up) {
  Vector3 f = forward.normalized();
  Vector3 u = up.normalized();
  Vector3 r = u.cross(f).normalized();
  u = f.cross(r);

  float m00 = r.x, m01 = r.y, m02 = r.z;
  float m10 = u.x, m11 = u.y, m12 = u.z;
  float m20 = f.x, m21 = f.y, m22 = f.z;

  float num8 = (m00 + m11) + m22;
  Quaternion quaternion;
  if (num8 > 0.0f) {
    float num = sqrt(num8 + 1.0f);
    quaternion.w = num * 0.5f;
    num = 0.5f / num;
    quaternion.x = (m12 - m21) * num;
    quaternion.y = (m20 - m02) * num;
    quaternion.z = (m01 - m10) * num;
    return quaternion;
  }
  if ((m00 >= m11) && (m00 >= m22)) {
    float num7 = sqrt(((1.0f + m00) - m11) - m22);
    float num4 = 0.5f / num7;
    quaternion.x = 0.5f * num7;
    quaternion.y = (m01 + m10) * num4;
    quaternion.z = (m02 + m20) * num4;
    quaternion.w = (m12 - m21) * num4;
    return quaternion;
  }
  if (m11 > m22) {
    float num6 = sqrt(((1.0f + m11) - m00) - m22);
    float num3 = 0.5f / num6;
    quaternion.x = (m10 + m01) * num3;
    quaternion.y = 0.5f * num6;
    quaternion.z = (m21 + m12) * num3;
    quaternion.w = (m20 - m02) * num3;
    return quaternion;
  }
  float num5 = sqrt(((1.0f + m22) - m00) - m11);
  float num2 = 0.5f / num5;
  quaternion.x = (m20 + m02) * num2;
  quaternion.y = (m21 + m12) * num2;
  quaternion.z = 0.5f * num5;
  quaternion.w = (m01 - m10) * num2;
  return quaternion;
}
Quaternion Quaternion::axisAngle(const Vector3 &axis, float angle) {
  Quaternion quaternion;
  float halfAngle = angle * 0.5f;
  float s = sinf(halfAngle);

  quaternion.w = cosf(halfAngle);
  quaternion.x = axis.x * s;
  quaternion.y = axis.y * s;
  quaternion.z = axis.z * s;

  return quaternion;
}
Quaternion Quaternion::fromEuler(float x, float y, float z) {
  float xRad = x * 0.0174532925f;
  float yRad = y * 0.0174532925f;
  float zRad = z * 0.0174532925f;
  float halfX = xRad * 0.5f;
  float halfY = yRad * 0.5f;
  float halfZ = zRad * 0.5f;

  float cosX = cosf(halfX);
  float sinX = sinf(halfX);
  float cosY = cosf(halfY);
  float sinY = sinf(halfY);
  float cosZ = cosf(halfZ);
  float sinZ = sinf(halfZ);

  Quaternion quaternion;
  quaternion.w = cosY * cosX * cosZ + sinY * sinX * sinZ;
  quaternion.x = cosY * sinX * cosZ + sinY * cosX * sinZ;
  quaternion.y = sinY * cosX * cosZ - cosY * sinX * sinZ;
  quaternion.z = cosY * cosX * sinZ - sinY * sinX * cosZ;
  return quaternion;
}
Vector3 Quaternion::toEuler() const {
  float sinr_cosp = 2 * (w * x + y * z);
  float cosr_cosp = 1 - 2 * (x * x + y * y);
  float roll = std::atan2(sinr_cosp, cosr_cosp);

  float sinp = 2 * (w * y - z * x);
  float pitch;
  if (std::fabs(sinp) >= 1) {
    pitch = std::copysign(3.141592653589793238f / 2, sinp);
  } else {
    pitch = std::asin(sinp);
  }

  float siny_cosp = 2 * (w * z + x * y);
  float cosy_cosp = 1 - 2 * (y * y + z * z);
  float yaw = std::atan2(siny_cosp, cosy_cosp);

  return {roll, pitch, yaw};
}
void Quaternion::toMatrix(float *matrix) const {
  // Assuming matrix is a 4x4 matrix in row-major order
  matrix[0] = 1 - 2 * (y * y + z * z);
  matrix[1] = 2 * (x * y - z * w);
  matrix[2] = 2 * (x * z + y * w);
  matrix[3] = 0;

  matrix[4] = 2 * (x * y + z * w);
  matrix[5] = 1 - 2 * (x * x + z * z);
  matrix[6] = 2 * (y * z - x * w);
  matrix[7] = 0;

  matrix[8] = 2 * (x * z - y * w);
  matrix[9] = 2 * (y * z + x * w);
  matrix[10] = 1 - 2 * (x * x + y * y);
  matrix[11] = 0;

  matrix[12] = 0;
  matrix[13] = 0;
  matrix[14] = 0;
  matrix[15] = 1;
}
