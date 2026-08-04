#include "skin/beatoraja/SkinResourceCatalog.h"
#include "view/ImageFileDecoder.h"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) { if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

void testBoundedPngAndJpegDecodeBeforeAllocation() {
  const std::filesystem::path resources =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests/fixtures/beatoraja_skin/resources";
  const auto png = image_decode::decodeImageFile(resources / "fixture.png", 40, 3200);
  const auto jpg = image_decode::decodeImageFile(resources / "fixture.jpg", 40, 3200);
  expect(png && png->width == 40 && png->height == 20 && png->byteSize() == 3200,
         "purpose-built PNG decodes to bounded RGBA");
  expect(jpg && jpg->width == 40 && jpg->height == 20 && jpg->byteSize() == 3200,
         "purpose-built JPEG decodes to bounded RGBA");
  expect(!image_decode::decodeImageFile(resources / "fixture.png", 39, 3200),
         "header dimensions exceeding policy are rejected before full decode");
}

void testSpriteBoundsAndNormalizedGridCells() {
  skin::SkinSourceRect authored{.x=0,.y=0,.w=40,.h=20,.gridColumn=2,.gridRow=0,.gridColumns=4,.gridRows=2};
  skin::SkinSourceRect resolved;
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 20 && resolved.y == 0 && resolved.w == 10 && resolved.h == 10,
         "row-major sprite preparation resolves the third grid cell for UV 0.5 to 0.75");
  authored.x = 31;
  expect(!skin::skinResourceResolveRect(authored, 40, 20, resolved),
         "out-of-bounds source crops are rejected before publication");
}
}

int main() {
  testBoundedPngAndJpegDecodeBeforeAllocation();
  testSpriteBoundsAndNormalizedGridCells();
  if (failures) return 1;
  std::cout << "Skin resource catalog tests passed\n";
  return 0;
}
