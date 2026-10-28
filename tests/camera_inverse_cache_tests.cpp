#include "rendering/Camera.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace rendering {
int window_width = 1280;
int window_height = 720;
}

// Camera's matrix math is production code. These upload-only boundaries keep
// the test independent of a GPU/device while exercising render() invalidation.
namespace bgfx {
Caps cameraTestCaps{};
const Caps *getCaps() { return &cameraTestCaps; }
void setViewTransform(ViewId, const void *, const void *) {}
void setViewRect(ViewId, int16_t, int16_t, uint16_t, uint16_t) {}
}

namespace {
struct CameraParameters {
  bx::Vec3 eye{0.0F, -2.0F, -8.0F};
  bx::Vec3 target{0.0F, 1.0F, 0.0F};
  bx::Vec3 up{0.0F, 1.0F, 0.0F};
  float fov = 75.0F;
  float aspect = 16.0F / 9.0F;
  float nearClip = 0.1F;
  float farClip = 80.0F;
};

void requireNear(const bx::Vec3 &actual, const bx::Vec3 &expected,
                 const char *message) {
  const auto near = [](float a, float b) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= 0.00001F * std::max(1.0F, std::abs(b));
  };
  if (!near(actual.x, expected.x) || !near(actual.y, expected.y) ||
      !near(actual.z, expected.z)) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bx::Vec3 uncachedDeprojection(const Camera &camera,
                             const CameraParameters &parameters,
                             float screenX, float screenY, float distance) {
  float view[16], projection[16], inverseView[16], inverseProjection[16];
  bx::mtxLookAt(view, parameters.eye, parameters.target, parameters.up);
  bx::mtxProj(projection, parameters.fov, parameters.aspect,
              parameters.nearClip, parameters.farClip,
              bgfx::cameraTestCaps.homogeneousDepth);
  bx::mtxInverse(inverseView, view);
  bx::mtxInverse(inverseProjection, projection);
  const bx::Vec3 ndc{
      2.0F * (screenX - camera.getViewX()) / camera.getViewWidth() - 1.0F,
      1.0F - 2.0F * (screenY - camera.getViewY()) / camera.getViewHeight(),
      1.0F};
  return bx::mul(bx::mul(bx::mul(ndc, inverseProjection), distance), inverseView);
}

void verify(const Camera &camera, const CameraParameters &parameters,
            const char *message) {
  for (float horizontal : {0.0F, 0.35F, 1.0F}) {
    for (float vertical : {0.0F, 0.6F, 1.0F}) {
      for (float distance : {parameters.nearClip, 3.0F, parameters.farClip}) {
        const float x = camera.getViewX() + horizontal * camera.getViewWidth();
        const float y = camera.getViewY() + vertical * camera.getViewHeight();
        requireNear(camera.deproject(x, y, distance),
                    uncachedDeprojection(camera, parameters, x, y, distance),
                    message);
      }
    }
  }
}

void verifyDepthConvention(bool homogeneousDepth) {
  bgfx::cameraTestCaps.homogeneousDepth = homogeneousDepth;
  CameraParameters parameters;
  Camera camera(0);
  camera.edit().setPosition(parameters.eye).setLookAt(parameters.target)
      .setUp(parameters.up).setFov(parameters.fov)
      .setAspectRatio(parameters.aspect).setNearClip(parameters.nearClip)
      .setFarClip(parameters.farClip).commit();
  camera.setViewRect(13, 29, 1280, 720);
  camera.render();
  verify(camera, parameters, "first inverse query after render matches uncached math");
  verify(camera, parameters, "repeated queries retain equivalent inverse matrices");

  parameters.eye = {1.5F, -3.0F, -9.0F};
  camera.setPosition(parameters.eye).render();
  verify(camera, parameters, "view update survives clearing upload dirtiness");
  parameters.fov = 95.0F;
  camera.setFov(parameters.fov).render();
  verify(camera, parameters, "projection update survives clearing upload dirtiness");
  parameters.target = {0.5F, 2.0F, 0.2F};
  camera.setLookAt(parameters.target);
  parameters.up = {0.1F, 1.0F, 0.0F};
  camera.setUp(parameters.up);
  verify(camera, parameters, "target and up setters invalidate the view inverse");

  const auto committed = parameters;
  parameters.eye = {-2.0F, -1.0F, -6.0F};
  parameters.aspect = 9.0F / 16.0F;
  parameters.nearClip = 0.3F;
  parameters.farClip = 120.0F;
  camera.edit().setPosition(parameters.eye).setAspectRatio(parameters.aspect)
      .setNearClip(parameters.nearClip).setFarClip(parameters.farClip);
  camera.render(true);
  verify(camera, committed, "uncommitted edits retain the committed matrices");
  camera.commit().render();
  verify(camera, parameters, "commit invalidates both inverse caches");

  parameters.nearClip = 0.7F;
  camera.setNearClip(parameters.nearClip);
  verify(camera, parameters, "near clip setter invalidates projection inverse");
  parameters.farClip = 180.0F;
  camera.setFarClip(parameters.farClip);
  verify(camera, parameters, "far clip setter invalidates projection inverse");
  parameters.aspect = 1.0F;
  camera.setAspectRatio(parameters.aspect);
  verify(camera, parameters, "aspect setter invalidates projection inverse");

  camera.setViewRect(61, 47, 600, 1000).render();
  verify(camera, parameters, "viewport changes use fresh normalization with cached inverses");
  camera.updateViewTransform().updateProjTransform().render();
  verify(camera, parameters, "explicit matrix updates preserve deprojection");
}
}

int main() {
  verifyDepthConvention(false);
  verifyDepthConvention(true);
  std::cout << "Camera inverse cache invalidation tests passed\n";
}
