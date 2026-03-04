//
// Created by XF on 9/2/2024.
//

#include "SpriteObject.h"
#include "bgfx/bgfx.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
void SpriteObject::renderImpl(RenderContext &context) {
  // Draw a 2D rectangle in 3D space
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  // Define the vertex layout
  bgfx::VertexLayout layout = rendering::PosTexCoord0Vertex::ms_decl;

  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);

  auto *vertices = (rendering::PosTexCoord0Vertex *)tvb.data;
  auto *index = (uint16_t *)tib.data;

  // Define the corners of the rectangle in local space (2D in X-Y plane)
  vertices[0] = {0.0f, height, 0.0f, 0.0f, 0.0f}; // x, y, z, u, v
  vertices[1] = {width, height, 0.0f, 1.0f * tileU, 0.0f};
  vertices[2] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f * tileV};
  vertices[3] = {width, 0.0f, 0.0f, 1.0f * tileU, 1.0f * tileV};

  // Set up indices for two triangles (quad)
  index[0] = 0;
  index[1] = 1;
  index[2] = 2;
  index[3] = 1;
  index[4] = 3;
  index[5] = 2;

  // Set up state (e.g., render state, texture, shaders)
  uint64_t state =
      BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA;
  bgfx::setState(state);

  // Set the vertex and index buffers
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texColor, texture);

  // Submit the draw call
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  bgfx::submit(rendering::main_view, kProgram);
}
void SpriteObject::setTexture(bgfx::TextureHandle texture) {
  this->texture = texture;
}
void SpriteObject::update(float dt) {}
