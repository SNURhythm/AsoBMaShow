//
// Created by XF on 8/30/2024.
//

#pragma once
#include "../math/Vector3.h"
#include "../math/Quaternion.h"
#include "../view/View.h"
struct Transform {
  Vector3 position;
  Quaternion rotation;
  Vector3 scale;
};
class GameObject {
public:
  Transform transform;
  GameObject();
  GameObject(Vector3 position, Quaternion rotation, Vector3 scale);
  GameObject(Vector3 position, Quaternion rotation);
  virtual ~GameObject() = default;
  GameObject(const GameObject &) = delete;
  GameObject &operator=(const GameObject &) = delete;
  GameObject(GameObject &&) = delete;
  GameObject &operator=(GameObject &&) = delete;
  explicit GameObject(Vector3 position);
  void translate(Vector3 translation);
  void rotate(Quaternion rotation);
  void scale(Vector3 scale);
  void setForward(Vector3 forward);
  void setUp(Vector3 up);
  void setRight(Vector3 right);
  void setAxis(Vector3 axis);
  void setAngle(float angle);
  void setAxisAngle(Vector3 axis, float angle);
  void lookAt(Vector3 target);
  void lookAt(Vector3 target, Vector3 up);

  virtual void update(float dt) = 0;
  void render(RenderContext &context) {
    if (visible) {
      applyTransform();
      renderImpl(context);
    }
  }
  void applyTransform() const;
  bool visible = true;

private:
  virtual void renderImpl(RenderContext &context) = 0;
};
