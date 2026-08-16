#pragma once

#include "skin/beatoraja/SkinModelValidator.h"

#include <vector>

namespace skin::test_support {

inline SkinModelValidationResult
validateWithAuthoredBuiltins(BeatorajaSkinModel model) {
  std::vector<SkinBuiltinBindingCatalogEntry> entries;
  const auto append = [&](const auto &bindings, const auto &typeFor) {
    for (const auto &binding : bindings) {
      if (const auto *builtin =
              std::get_if<SkinBuiltinPropertySelector>(&binding.source)) {
        entries.push_back({.type = typeFor(binding), .selector = *builtin});
      }
    }
  };
  append(model.booleanProperties, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::BooleanProperty};
  });
  append(model.integerProperties, [](const auto &binding) {
    return SkinBindingType{.kind = SkinBindingKind::IntegerProperty,
                           .integerDomain = binding.domain};
  });
  append(model.floatProperties, [](const auto &binding) {
    return SkinBindingType{.kind = SkinBindingKind::FloatProperty,
                           .floatDomain = binding.domain};
  });
  append(model.stringProperties, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::StringProperty};
  });
  append(model.timerProperties, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::TimerProperty};
  });
  append(model.floatWriters, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::FloatWriter};
  });
  append(model.stringWriters, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::StringWriter};
  });
  append(model.events, [](const auto &) {
    return SkinBindingType{.kind = SkinBindingKind::Event};
  });
  return SkinModelValidator{}.validate(
      std::move(model),
      {.builtins = SkinBuiltinBindingCatalogView(entries), .callbacks = {}});
}

} // namespace skin::test_support
