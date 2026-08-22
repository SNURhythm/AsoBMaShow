#pragma once

#include "BeatorajaSkinModel.h"
#include "Lr2SkinCsvParser.h"

#include <span>
#include <string_view>

namespace skin {

class Lr2SkinHeaderDecoder final {
public:
  [[nodiscard]] HeaderDecodeResult
  decode(std::span<const Lr2SkinCommand>,
         std::string_view skinPath = "skin") const;
};

} // namespace skin
