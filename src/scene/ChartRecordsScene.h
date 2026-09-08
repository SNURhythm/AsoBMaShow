#pragma once

#include "../repositories/ChartRepository.h"
#include "Scene.h"
#include "SceneReturnTarget.h"

#include <utility>

class ResultRecordListView;
class TextView;
class View;

class ChartRecordsScene final : public Scene {
public:
  ChartRecordsScene(ApplicationContext &context, ChartMetaRecord record,
                    SceneReturnTarget returnTarget)
      : Scene(context), record_(std::move(record)),
        returnTarget_(std::move(returnTarget)) {}

  void init() override;
  EventHandleResult handleEvents(SDL_Event &) override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void buildView();
  void loadRecords();
  void goBack();

  ChartMetaRecord record_;
  SceneReturnTarget returnTarget_;
  View *rootLayout_ = nullptr;
  ResultRecordListView *recordsView_ = nullptr;
  TextView *emptyText_ = nullptr;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
};
