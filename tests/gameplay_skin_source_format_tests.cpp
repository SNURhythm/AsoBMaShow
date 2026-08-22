#include "skin/beatoraja/GameplaySkinSourceFormat.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testGameplaySkinSourceFormatClassification() {
  expect(skin::gameplaySkinSourceFormatForPath("Play/main.luaskin") ==
             skin::GameplaySkinSourceFormat::Lua,
         "luaskin must classify as Lua");
  expect(skin::gameplaySkinSourceFormatForPath("Play/main.JSON") ==
             skin::GameplaySkinSourceFormat::Json,
         "extension matching must be ASCII case-insensitive");
  expect(skin::gameplaySkinSourceFormatForPath("Play/main.lr2skin") ==
             skin::GameplaySkinSourceFormat::Lr2,
         "lr2skin must classify as LR2");
  expect(!skin::gameplaySkinSourceFormatForPath("config.json.bak"),
         "suffix-like names must not classify");
}

} // namespace

int main() {
  testGameplaySkinSourceFormatClassification();
  std::cout << "gameplay skin source format tests passed\n";
  return 0;
}
