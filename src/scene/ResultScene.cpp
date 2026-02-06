#include "ResultScene.h"
#include "../view/Button.h"
#include "../view/TextView.h"

#include "../rendering/Color.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/common.h"
#include "bgfx/bgfx.h"
#include "../skin/DefaultSkin.h"
#include "../skin/SkinTypes.h"

static void drawRect(float x, float y, float width, float height, Color color) {
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::VertexLayout layout = rendering::PosColorVertex::ms_decl;

  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);

  auto *vertices = (rendering::PosColorVertex *)tvb.data;
  auto *index = (uint16_t *)tib.data;

  uint32_t abgr = color.toABGR();
  vertices[0] = {x, y, 0.0f, abgr};
  vertices[1] = {x + width, y, 0.0f, abgr};
  vertices[2] = {x + width, y + height, 0.0f, abgr};
  vertices[3] = {x, y + height, 0.0f, abgr};

  index[0] = 0;
  index[1] = 1;
  index[2] = 2;
  index[3] = 2;
  index[4] = 3;
  index[5] = 0;

  uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
  bgfx::setState(state);

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  bgfx::submit(
      rendering::main_view,
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE));
}



// ... (drawRect function remains the same)

ResultScene::ResultScene(ApplicationContext &context,
                         const bms_parser::ChartMeta &meta,
                         const RhythmState &state)
    : Scene(context), meta(meta), resultState(state) {
        skin = new DefaultSkin();
    }

ResultScene::~ResultScene() { 
    delete rootLayout; 
    delete skin;
}

void ResultScene::init() {
  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  
  ResultSkinData data = { &resultState, &meta, &context };
  skin->buildLayout("Result", rootLayout, &data);

  graphPlaceHolder = rootLayout->findViewByName("graph");

  addView(rootLayout);
  rootLayout->applyYogaLayout();
}

void ResultScene::update(float dt) {}

void ResultScene::renderScene() {
  // Draw Gauge Graph
  if (graphPlaceHolder && !resultState.gaugeHistory.empty()) {
    float x = graphPlaceHolder->getX();
    float y = graphPlaceHolder->getY();
    float w = graphPlaceHolder->getWidth();
    float h = graphPlaceHolder->getHeight();

    // Draw background
    drawRect(x, y, w, h, Color(50, 50, 50, 200));

    // Draw graph
    size_t count = resultState.gaugeHistory.size();
    if (count > 1) {
      bgfx::TransientVertexBuffer tvb{};
      bgfx::TransientIndexBuffer tib{};

      if (bgfx::getAvailTransientVertexBuffer(count * 4,
                                              rendering::PosColorVertex::ms_decl) ==
              count * 4 &&
          bgfx::getAvailTransientIndexBuffer(count * 6) == count * 6) {

        bgfx::allocTransientVertexBuffer(
            &tvb, count * 4, rendering::PosColorVertex::ms_decl);
        bgfx::allocTransientIndexBuffer(&tib, count * 6);

        auto *vertices = (rendering::PosColorVertex *)tvb.data;
        auto *index = (uint16_t *)tib.data;

        float step = w / static_cast<float>(count);

        for (size_t i = 0; i < count; ++i) {
          float val = resultState.gaugeHistory[i]; // 0 to 100
          float barH = (val / 100.0f) * h;
          // Color based on value
          Color barColor = val > 80.0f ? Color(0, 255, 255, 200)
                                       : (val > 30.0f ? Color(0, 255, 0, 200)
                                                      : Color(255, 0, 0, 200));
          uint32_t abgr = barColor.toABGR();
          float bx = x + i * step;
          float by = y + h - barH;

          vertices[i * 4 + 0] = {bx, by, 0.0f, abgr};
          vertices[i * 4 + 1] = {bx + step, by, 0.0f, abgr};
          vertices[i * 4 + 2] = {bx + step, by + barH, 0.0f, abgr};
          vertices[i * 4 + 3] = {bx, by + barH, 0.0f, abgr};

          index[i * 6 + 0] = i * 4 + 0;
          index[i * 6 + 1] = i * 4 + 1;
          index[i * 6 + 2] = i * 4 + 2;
          index[i * 6 + 3] = i * 4 + 2;
          index[i * 6 + 4] = i * 4 + 3;
          index[i * 6 + 5] = i * 4 + 0;
        }

        uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                         BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
        bgfx::setState(state);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::submit(
            rendering::ui_view,
            rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE));
      } else {
        // Fallback or just don't draw if too many
        SDL_Log("Too many points in gauge history to draw: %zu", count);
      }
    }
  }
}

void ResultScene::cleanupScene() {
    // Resources are cleaned up by Scene logic usually, but we have Views.
    // Scene::cleanup() deletes all views.
}
