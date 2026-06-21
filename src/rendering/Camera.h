#include <bgfx/bgfx.h>
#include <bx/math.h>
#include "./common.h"

class Camera {
public:
  explicit Camera(bgfx::ViewId viewId);
  ~Camera();
  Camera &setPosition(bx::Vec3 position);
  Camera &setLookAt(bx::Vec3 lookAt);
  Camera &setUp(bx::Vec3 up);
  Camera &setFov(float fov);
  Camera &setViewId(bgfx::ViewId viewId);
  Camera &setViewRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
  Camera &setAspectRatio(float aspectRatio);
  Camera &setNearClip(float nearClip);
  Camera &setFarClip(float farClip);
  Camera &updateViewTransform();
  Camera &updateProjTransform();
  Camera &edit();
  Camera &commit();
  void render(bool force = false);
  bx::Vec3 project(bx::Vec3 point) const;
  bx::Vec3 deproject(float x, float y, float distance);
  float getDistanceFromEye(bx::Vec3 point);
  [[nodiscard]] float getNearClip() const { return nearClip; }
  [[nodiscard]] float getFarClip() const { return farClip; }
  [[nodiscard]] bx::Vec3 getEye() const { return eye; }
  [[nodiscard]] bx::Vec3 getLookAt() const { return lookAt; }
  [[nodiscard]] uint16_t getViewX() const { return x; }
  [[nodiscard]] uint16_t getViewY() const { return y; }
  [[nodiscard]] uint16_t getViewWidth() const { return width; }
  [[nodiscard]] uint16_t getViewHeight() const { return height; }

private:
  bool editing = false;
  bool viewMtxDirty = true;
  bool projMtxDirty = true;
  bool rectDirty = true;
  float viewMtx[16];
  float projMtx[16];
  bx::Vec3 up = {0.0f, 1.0f, 0.0f};
  bx::Vec3 eye = {0.0f, 0.0f, 0.0f};
  bx::Vec3 lookAt = {0.0f, 0.0f, 0.0f};
  bgfx::ViewId viewId;
  float fov = 120.0f;
  float aspectRatio = 1.0f;
  float nearClip = 0.1f;
  float farClip = 100.0f;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = rendering::window_width;
  uint16_t height = rendering::window_height;
};
