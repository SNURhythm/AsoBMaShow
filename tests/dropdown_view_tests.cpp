#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/DropdownView.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_base_of_v<View, DropdownView>);
static_assert(std::is_same_v<decltype(DropdownView::State{}.options),
                             std::vector<DropdownView::Option>>);
static_assert(
    std::is_same_v<decltype(DropdownView::State{}.selectedId), std::string>);
static_assert(std::is_same_v<decltype(DropdownView::State{}.enabled), bool>);
static_assert(std::is_same_v<decltype(DropdownView::Callbacks{}.onOpenChanged),
                             std::function<void(bool)>>);
static_assert(DropdownView::kDefaultWidth == 160.0f);
static_assert(std::is_same_v<decltype(DropdownView::refreshIndicator(
                                 std::declval<View *>(),
                                 std::declval<const std::optional<Color> &>())),
                             bool>);

int main() { return 0; }
