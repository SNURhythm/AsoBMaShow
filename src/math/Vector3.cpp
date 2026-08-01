//
// Created by XF on 8/30/2024.
//

#include <cmath>
#include "Vector3.h"
#include "Quaternion.h"
Vector3 Vector3::operator+(Vector3 other) const {
  return {x + other.x, y + other.y, z + other.z};
}
Vector3 Vector3::operator-(Vector3 other) const {
  return {x - other.x, y - other.y, z - other.z};
}
Vector3 Vector3::operator*(float scalar) const {
  return {x * scalar, y * scalar, z * scalar};
}
Vector3 Vector3::operator/(float scalar) const {
  return {x / scalar, y / scalar, z / scalar};
}
Vector3 &Vector3::operator+=(Vector3 &other) {
  x += other.x;
  y += other.y;
  z += other.z;
  return *this;
}
Vector3 &Vector3::operator-=(Vector3 &other) {
  x -= other.x;
  y -= other.y;
  z -= other.z;
  return *this;
}
Vector3 &Vector3::operator*=(float scalar) {
  x *= scalar;
  y *= scalar;
  z *= scalar;
  return *this;
}
Vector3 &Vector3::operator/=(float scalar) {
  x /= scalar;
  y /= scalar;
  z /= scalar;
  return *this;
}
bool Vector3::operator==(Vector3 &other) const {
  return x == other.x && y == other.y && z == other.z;
}
bool Vector3::operator!=(Vector3 &other) const {
  return x != other.x || y != other.y || z != other.z;
}
float Vector3::dot(const Vector3 &other) const {
  return x * other.x + y * other.y + z * other.z;
}
Vector3 Vector3::cross(const Vector3 &other) const {
  return {y * other.z - z * other.y, z * other.x - x * other.z,
          x * other.y - y * other.x};
}
float Vector3::magnitude() const { return sqrtf(x * x + y * y + z * z); }
Vector3 Vector3::normalized() const {
  float mag = magnitude();
  return {x / mag, y / mag, z / mag};
}
float Vector3::distance(const Vector3 &other) const {
  return (*this - other).magnitude();
}
float Vector3::angle(const Vector3 &other) const {
  return acosf(dot(other) / (magnitude() * other.magnitude()));
}
Vector3 Vector3::lerp(const Vector3 &other, float t) const {
  return *this * (1 - t) + other * t;
}
Vector3 Vector3::slerp(const Vector3 &other, float t) const {
  float theta = angle(other);
  return (*this * sinf((1 - t) * theta) + other * sinf(t * theta)) /
         sinf(theta);
}
Vector3 Vector3::project(const Vector3 &other) const {
  return other * (dot(other) / other.dot(other));
}
Vector3 Vector3::reflect(const Vector3 &normal) const {
  return *this - normal * 2 * dot(normal);
}
Vector3 Vector3::refract(const Vector3 &normal, float eta) const {
  float cosI = -dot(normal);
  float sinT2 = eta * eta * (1 - cosI * cosI);
  if (sinT2 > 1) {
    return {0, 0, 0};
  }
  float cosT = sqrtf(1 - sinT2);
  return *this * eta + normal * (eta * cosI - cosT);
}
Vector3 Vector3::rotate(const Vector3 &axis, float angle) const {
  float sinHalfAngle = sinf(angle / 2);
  float cosHalfAngle = cosf(angle / 2);
  float rX = axis.x * sinHalfAngle;
  float rY = axis.y * sinHalfAngle;
  float rZ = axis.z * sinHalfAngle;
  float rW = cosHalfAngle;
  Quaternion rotation = {rX, rY, rZ, rW};
  Quaternion conjugate = rotation.conjugate();
  Quaternion w = rotation * (*this) * conjugate;
  return {w.x, w.y, w.z};
}
Vector3 &Vector3::operator*=(Vector3 &other) {
  const Vector3 result = cross(other);
  x = result.x;
  y = result.y;
  z = result.z;
  return *this;
}
