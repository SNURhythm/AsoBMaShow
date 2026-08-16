#include "scene/play/BMSRenderer.h"
#include "scene/play/BuiltInPlayfieldPresentation.h"

#include <optional>
#include <type_traits>

namespace {

using BeginLaneCoverDrag =
    std::optional<float> (BuiltInPlayfieldPresentation::*)(float,
                                                           float) const;
using UpdateLaneCoverDrag =
    int (BuiltInPlayfieldPresentation::*)(float, float, float);

static_assert(std::is_same_v<
              decltype(&BuiltInPlayfieldPresentation::laneCoverHandleGrabOffset),
              BeginLaneCoverDrag>);
static_assert(std::is_same_v<
              decltype(&BuiltInPlayfieldPresentation::dragLaneCoverHandleTo),
              UpdateLaneCoverDrag>);
static_assert(std::is_same_v<
              decltype(BuiltInPlayfieldPresentationCreateInfo::replayData),
              const ReplayData *>);
static_assert(!std::is_abstract_v<BMSRenderer>);

} // namespace

int main() { return 0; }
