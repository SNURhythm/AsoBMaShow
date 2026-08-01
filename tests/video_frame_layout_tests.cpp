#include "video/VideoFrameLayout.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testRejectsInvalidAndUnrepresentableDimensions() {
  expect(!video::makeYuv420FrameLayout(0, 1), "zero width is rejected");
  expect(!video::makeYuv420FrameLayout(1, 0), "zero height is rejected");
  expect(!video::makeYuv420FrameLayout(-1, 1), "negative width is rejected");
  expect(!video::makeYuv420FrameLayout(1, -1), "negative height is rejected");
  expect(!video::makeYuv420FrameLayout(65'536, 1),
         "dimensions wider than a bgfx texture are rejected");
  expect(!video::makeYuv420FrameLayout(1, 65'536),
         "dimensions taller than a bgfx texture are rejected");
  expect(!video::makeYuv420FrameLayout(
             std::numeric_limits<std::int64_t>::max(),
             std::numeric_limits<std::int64_t>::max()),
         "overflow-scale dimensions are rejected");
}

void testOnePixelFrameKeepsChromaPlanes() {
  const auto layout = video::makeYuv420FrameLayout(1, 1);
  expect(layout.has_value(), "1x1 YUV420 layout is valid");
  if (!layout) {
    return;
  }
  expect(layout->width == 1 && layout->height == 1,
         "1x1 luma dimensions are preserved");
  expect(layout->chromaWidth == 1 && layout->chromaHeight == 1,
         "1x1 frame has non-empty chroma planes");
  expect(layout->yPitch == 1 && layout->uPitch == 1 && layout->vPitch == 1,
         "tight 1x1 pitches are one byte");
  expect(layout->yBytes == 1 && layout->uBytes == 1 && layout->vBytes == 1 &&
             layout->totalBytes == 3,
         "1x1 byte counts include every plane");
}

void testEvenAndOddDimensionsUseCeilingChromaDivision() {
  const auto even = video::makeYuv420FrameLayout(4, 6);
  expect(even && even->chromaWidth == 2 && even->chromaHeight == 3 &&
             even->yBytes == 24 && even->uBytes == 6 &&
             even->vBytes == 6 && even->totalBytes == 36,
         "even dimensions retain conventional YUV420 layout");

  const auto oddWidth = video::makeYuv420FrameLayout(3, 4);
  expect(oddWidth && oddWidth->chromaWidth == 2 &&
             oddWidth->chromaHeight == 2 && oddWidth->yBytes == 12 &&
             oddWidth->uBytes == 4 && oddWidth->vBytes == 4,
         "odd width rounds chroma width upward");

  const auto oddHeight = video::makeYuv420FrameLayout(4, 5);
  expect(oddHeight && oddHeight->chromaWidth == 2 &&
             oddHeight->chromaHeight == 3 && oddHeight->yBytes == 20 &&
             oddHeight->uBytes == 6 && oddHeight->vBytes == 6,
         "odd height rounds chroma height upward");

  const auto bothOdd = video::makeYuv420FrameLayout(3, 5);
  expect(bothOdd && bothOdd->chromaWidth == 2 &&
             bothOdd->chromaHeight == 3 && bothOdd->totalBytes == 27,
         "both odd dimensions are represented without truncation");
}

void testPaddedPitchesAreCheckedAndAccounted() {
  const auto padded = video::makeYuv420FrameLayout(3, 5, 8, 4, 4);
  expect(padded && padded->yPitch == 8 && padded->uPitch == 4 &&
             padded->vPitch == 4 && padded->yBytes == 40 &&
             padded->uBytes == 12 && padded->vBytes == 12 &&
             padded->totalBytes == 64,
         "padded decoder pitches determine copy byte counts");
  expect(!video::makeYuv420FrameLayout(3, 5, 2, 2, 2),
         "pitch narrower than its plane is rejected");
  expect(!video::makeYuv420FrameLayout(
             1, 65'535, std::numeric_limits<std::int64_t>::max(), 1, 1),
         "unrepresentable pitch multiplication is rejected");
}

} // namespace

int main() {
  testRejectsInvalidAndUnrepresentableDimensions();
  testOnePixelFrameKeepsChromaPlanes();
  testEvenAndOddDimensionsUseCeilingChromaDivision();
  testPaddedPitchesAreCheckedAndAccounted();
  if (failures != 0) {
    std::cerr << failures << " video frame layout test(s) failed\n";
    return 1;
  }
  std::cout << "Video frame layout tests passed\n";
  return 0;
}
