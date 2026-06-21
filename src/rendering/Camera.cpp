#include "Camera.h"

Camera::Camera(bgfx::ViewId viewId) : viewId(viewId) {}
Camera::~Camera() {}
Camera &Camera::setViewId(bgfx::ViewId viewId) {
  this->viewId = viewId;
  return *this;
}

Camera &Camera::setViewRect(uint16_t x, uint16_t y, uint16_t width,
                            uint16_t height) {
  this->x = x;
  this->y = y;
  this->width = width;
  this->height = height;
  rectDirty = true;
  return *this;
}

Camera &Camera::setPosition(bx::Vec3 position) {
  this->eye = position;
  if (!editing)
    updateViewTransform();
  return *this;
}

Camera &Camera::setLookAt(bx::Vec3 lookAt) {
  this->lookAt = lookAt;
  if (!editing)
    updateViewTransform();
  return *this;
}

Camera &Camera::setUp(bx::Vec3 up) {
  this->up = up;
  if (!editing)
    updateViewTransform();
  return *this;
}

Camera &Camera::setFov(float fov) {
  this->fov = fov;
  if (!editing)
    updateProjTransform();
  return *this;
}

Camera &Camera::setAspectRatio(float aspectRatio) {
  this->aspectRatio = aspectRatio;
  if (!editing)
    updateProjTransform();
  return *this;
}

Camera &Camera::setNearClip(float nearClip) {
  this->nearClip = nearClip;
  if (!editing)
    updateProjTransform();
  return *this;
}

Camera &Camera::setFarClip(float farClip) {
  this->farClip = farClip;
  if (!editing)
    updateProjTransform();
  return *this;
}

Camera &Camera::updateViewTransform() {
  bx::mtxLookAt(viewMtx, eye, lookAt, up);
  viewMtxDirty = true;
  return *this;
}

Camera &Camera::updateProjTransform() {
  bx::mtxProj(projMtx, fov, aspectRatio, nearClip, farClip,
              bgfx::getCaps()->homogeneousDepth);
  projMtxDirty = true;
  return *this;
}

void Camera::render(bool force) {
  if (projMtxDirty || viewMtxDirty || force) {
    bgfx::setViewTransform(viewId, viewMtx, projMtx);
    projMtxDirty = false;
    viewMtxDirty = false;
  }
  if (rectDirty || force) {
    bgfx::setViewRect(viewId, x, y, width, height);
    rectDirty = false;
  }
}

Camera &Camera::edit() {
  editing = true;
  return *this;
}

Camera &Camera::commit() {
  editing = false;
  updateViewTransform();
  updateProjTransform();
  return *this;
}

bx::Vec3 Camera::project(bx::Vec3 point) const {
  const bx::Vec3 viewPoint = bx::mul(point, viewMtx);
  const bx::Vec3 ndcPoint = bx::mulH(viewPoint, projMtx);
  return {static_cast<float>(x) + (ndcPoint.x + 1.0f) * 0.5f *
                                      static_cast<float>(width),
          static_cast<float>(y) + (1.0f - ndcPoint.y) * 0.5f *
                                      static_cast<float>(height),
          ndcPoint.z};
}

bx::Vec3 Camera::deproject(float x, float y, float distance) {
  // First, normalize screen coordinates to NDC (Normalized Device Coordinates)
  float ndcX = (2.0f * (x - this->x) / this->width) - 1.0f;
  float ndcY = 1.0f - (2.0f * (y - this->y) / this->height);

  // Create a point in NDC space at the specified distance along the camera's
  // view direction
  bx::Vec3 pointInNDC(ndcX, ndcY, 1.0f);

  // Multiply by the inverse projection matrix to get the point in view space
  float invProjMtx[16];
  bx::mtxInverse(invProjMtx, projMtx);
  bx::Vec3 pointInView = bx::mul(pointInNDC, invProjMtx);

  // Scale by the distance along the view direction
  pointInView = bx::mul(pointInView, distance);

  // Multiply by the inverse view matrix to get the point in world space
  float invViewMtx[16];
  bx::mtxInverse(invViewMtx, viewMtx);
  bx::Vec3 pointInWorld = bx::mul(pointInView, invViewMtx);

  return pointInWorld;
}

float Camera::getDistanceFromEye(bx::Vec3 point) {
  bx::Vec3 eyeToPoint = {point.x - eye.x, point.y - eye.y, point.z - eye.z};
  return bx::length(eyeToPoint);
}
