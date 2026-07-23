#pragma once

#include "View.h"

#include <string>

class TextView;

class CheckboxButtonContent final : public View {
public:
  CheckboxButtonContent(std::string label, int labelSize, int iconSize);

  void setChecked(bool checked);
  [[nodiscard]] bool checked() const noexcept { return checked_; }
  void setThemedColor(ThemeColorProvider provider);

  [[nodiscard]] TextView *iconView() { return icon_; }
  [[nodiscard]] const TextView *iconView() const { return icon_; }
  [[nodiscard]] TextView *labelView() { return label_; }
  [[nodiscard]] const TextView *labelView() const { return label_; }

private:
  TextView *icon_ = nullptr;
  TextView *label_ = nullptr;
  bool checked_ = false;
};
