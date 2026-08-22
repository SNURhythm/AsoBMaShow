#pragma once

#include <vector>

enum class ResultTouchControlAction : unsigned char {
  Back,
  Retry,
  RetrySame,
  Replay,
  Rankings,
  ExportPhoto,
  SelectSection,
  Next,
  Hide,
};

struct ResultTouchControlAvailability {
  bool back = false;
  bool retry = false;
  bool retrySame = false;
  bool replay = false;
  bool rankings = false;
  bool exportPhoto = false;
  bool selectSection = false;
  bool next = false;
};

struct ResultTouchControlState {
  bool touchControlsEnabled = false;
  bool skinSelected = false;
  bool hidden = false;
};

struct ResultTouchControlPresentation {
  bool showsControls = false;
  bool capturesRestoreTouch = false;
  std::vector<ResultTouchControlAction> actions;
};

inline ResultTouchControlPresentation makeResultTouchControlPresentation(
    const ResultTouchControlState &state,
    const ResultTouchControlAvailability &availability) {
  if (!state.touchControlsEnabled || !state.skinSelected) {
    return {};
  }
  if (state.hidden) {
    return {.capturesRestoreTouch = true};
  }
  ResultTouchControlPresentation result{.showsControls = true};
  if (availability.back) result.actions.push_back(ResultTouchControlAction::Back);
  if (availability.retry) result.actions.push_back(ResultTouchControlAction::Retry);
  if (availability.retrySame) result.actions.push_back(ResultTouchControlAction::RetrySame);
  if (availability.replay) result.actions.push_back(ResultTouchControlAction::Replay);
  if (availability.rankings) result.actions.push_back(ResultTouchControlAction::Rankings);
  if (availability.exportPhoto) result.actions.push_back(ResultTouchControlAction::ExportPhoto);
  if (availability.selectSection) result.actions.push_back(ResultTouchControlAction::SelectSection);
  if (availability.next) result.actions.push_back(ResultTouchControlAction::Next);
  result.actions.push_back(ResultTouchControlAction::Hide);
  return result;
}
