#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinBindingDecoder.h"

namespace skin {

class SkinModelValidator final {
public:
  SkinModelValidationResult validate(BeatorajaSkinModel,
                                     SkinBindingValidationContext) const;
};

} // namespace skin
