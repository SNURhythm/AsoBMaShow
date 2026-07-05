#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/scene/ChartFilterSortPanelView.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <string_view>
#include <type_traits>
#include <utility>

constexpr bool chartSortOptionsContain(ChartRecordSortCriterion criterion) {
  for (const auto &option : ChartSortPanelView::kSortOptions) {
    if (option.criterion == criterion) {
      return true;
    }
  }
  return false;
}

constexpr bool chartSortOptionsContainLabel(std::string_view label) {
  for (const auto &option : ChartSortPanelView::kSortOptions) {
    if (std::string_view(option.label) == label) {
      return true;
    }
  }
  return false;
}

static_assert(std::is_base_of_v<View, ChartFilterPanelView>);
static_assert(std::is_base_of_v<View, ChartSortPanelView>);
static_assert(std::is_same_v<decltype(ChartFilterPanelView::State{}.filters),
                             ChartRecordFilters>);
static_assert(
    std::is_same_v<decltype(ChartFilterPanelView::State{}
                                .clearMarkFilterVisible),
                   bool>);
static_assert(std::is_same_v<decltype(ChartFilterPanelView::State{}
                                          .effectiveClearMarkRank),
                             std::optional<int>>);
static_assert(std::is_same_v<decltype(ChartSortPanelView::State{}.sort),
                             ChartRecordSortState>);
static_assert(
    std::is_same_v<decltype(std::declval<View &>().setDisplay(YGDisplayFlex)),
                   View *>);
static_assert(std::is_same_v<
              decltype(ChartSortPanelView::SortButton{}.cell), View *>);
static_assert(std::is_same_v<
              decltype(std::declval<ChartSortPanelView &>().difficultySortCell),
              View *>);
static_assert(std::is_same_v<decltype(std::declval<ChartSortPanelView &>()
                                          .difficultySortButton),
                             Button *>);
static_assert(std::is_same_v<decltype(std::declval<ChartSortPanelView &>()
                                          .sortGridRows),
                             std::vector<View *>>);
static_assert(!chartSortOptionsContainLabel("Max Combo"));
static_assert(chartSortOptionsContain(ChartRecordSortCriterion::MainBpm));

int main() { return 0; }
