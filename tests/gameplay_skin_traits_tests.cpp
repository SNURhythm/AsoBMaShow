#include "skin/GameplaySkinTraits.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testPinnedBeatorajaGameplayTraitMapping() {
  const auto ten = skin::gameplaySkinTraitForSkinType(3);
  require(ten.has_value() && ten->keyMode == 10 && ten->label == "10K",
          "Beatoraja play10 type maps to 10K");

  const auto double24 = skin::gameplaySkinTraitForKeyMode(48);
  require(double24.has_value() && double24->skinType == 17 &&
              double24->label == "24K Double",
          "Beatoraja 24K Double maps to a 48-key chart");

  require(!skin::gameplaySkinTraitForSkinType(5).has_value(),
          "non-gameplay skin types are not gameplay traits");
  require(!skin::gameplaySkinTraitForKeyMode(14)->label.empty(),
          "every supported gameplay key mode has a visible label");
}

} // namespace

int main() {
  testPinnedBeatorajaGameplayTraitMapping();
  return 0;
}
