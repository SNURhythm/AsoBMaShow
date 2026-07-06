#pragma once

#include "../rendering/Color.h"
#include "View.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ScrollView;
class TextView;

class DropdownView : public View {
public:
  static constexpr float kDefaultWidth = 160.0f;

  struct Option {
    std::string id;
    std::string label;
    std::optional<Color> leadingColor;
  };

  struct State {
    std::string label;
    std::string selectedId;
    std::vector<Option> options;
    bool open = false;
    bool enabled = true;
    int maxVisibleItems = 6;
    float menuWidth = 0.0f;
  };

  struct Callbacks {
    std::function<void(bool)> onOpenChanged;
    std::function<void(const std::string &)> onOptionSelected;
  };

  explicit DropdownView(Callbacks callbacks);
  ~DropdownView() override;

  void refresh(const State &state);
  void onLayout() override;

private:
  struct OptionButton {
    Button *button = nullptr;
    View *indicator = nullptr;
    TextView *text = nullptr;
    std::string id;
    std::optional<Color> leadingColor;
  };

  Callbacks callbacks;
  State current;
  Button *triggerButton = nullptr;
  View *triggerIndicator = nullptr;
  TextView *triggerText = nullptr;
  TextView *triggerIcon = nullptr;
  ScrollView *menuScroll = nullptr;
  View *menuContent = nullptr;
  std::vector<OptionButton> optionButtons;
  std::optional<State> pendingRefresh;
  std::shared_ptr<bool> lifetimeToken = std::make_shared<bool>(true);
  bool placementUpdating = false;
  bool dispatchingOptionCallback = false;
  bool deferredRefreshScheduled = false;

  void buildView();
  void applyRefresh(State state);
  void rebuildOptions();
  void refreshVisualState();
  void updateMenuPlacement();
  void scheduleDeferredRefresh();
  [[nodiscard]] std::string selectedLabel() const;
  [[nodiscard]] std::optional<Color> selectedLeadingColor() const;
  [[nodiscard]] bool optionsMatch(const std::vector<Option> &options) const;
  [[nodiscard]] bool refreshRequiresOptionRebuild(const State &state) const;
  [[nodiscard]] bool pointInsideOpenArea(float uiX, float uiY) const;
  static bool refreshIndicator(View *indicator,
                               const std::optional<Color> &color);
  bool handleEventsImpl(SDL_Event &event) override;
};
