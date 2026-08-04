#include "video/VideoFrameLayout.h"

#include <array>
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

void testEmbeddedYuvQuadPreservesArbitraryDestinationPoints() {
  // The canonical skin/renderer contract is BL, BR, TR, TL.
  const std::array<video::VideoQuadPoint, 4> destinations = {{
      {4.25F, 38.0F},
      {52.75F, 46.5F},
      {47.0F, 9.5F},
      {11.5F, -3.25F},
  }};
  const std::array<video::VideoQuadPoint, 4> uvs = {{
      {0.0F, 1.0F},
      {1.0F, 1.0F},
      {1.0F, 0.0F},
      {0.0F, 0.0F},
  }};
  const video::VideoQuadTint tint{-0.5F, 1.25F, 2.0F, 0.4F};

  const auto layout = video::makeEmbeddedYuvQuadLayout(destinations, uvs, tint);
  expect(layout.has_value(), "arbitrary destination quad is accepted");
  if (!layout) {
    return;
  }

  for (std::size_t i = 0; i < destinations.size(); ++i) {
    expect(layout->vertices[i].x == destinations[i].x &&
               layout->vertices[i].y == destinations[i].y &&
               layout->vertices[i].u == uvs[i].x &&
               layout->vertices[i].v == uvs[i].y &&
               layout->vertices[i].r == tint.r && layout->vertices[i].g == tint.g &&
               layout->vertices[i].b == tint.b && layout->vertices[i].a == tint.a,
           "distinct BL/BR/TR/TL destination and UV values pass through without permutation");
  }
}

void testEmbeddedYuvQuadPreservesTrimmedUvs() {
  const std::array<video::VideoQuadPoint, 4> destinations = {{
      {0.0F, 50.0F},
      {100.0F, 50.0F},
      {100.0F, 0.0F},
      {0.0F, 0.0F},
  }};
  const std::array<video::VideoQuadPoint, 4> trimmedUvs = {{
      {0.125F, 0.75F},
      {0.875F, 0.75F},
      {0.875F, 0.25F},
      {0.125F, 0.25F},
  }};

  const auto layout = video::makeEmbeddedYuvQuadLayout(
      destinations, trimmedUvs, {0.25F, 0.5F, 1.5F, 0.75F});
  expect(layout.has_value(), "trimmed UV quad is accepted");
  if (!layout) {
    return;
  }

  for (std::size_t i = 0; i < trimmedUvs.size(); ++i) {
    expect(layout->vertices[i].u == trimmedUvs[i].x &&
               layout->vertices[i].v == trimmedUvs[i].y,
           "caller-resolved trimmed UVs are preserved exactly");
  }
}

void testEmbeddedYuvQuadReplicatesTintAndUsesFixedWinding() {
  const std::array<video::VideoQuadPoint, 4> points = {{
      {-2.0F, 13.0F},
      {8.0F, 13.0F},
      {8.0F, 3.0F},
      {-2.0F, 3.0F},
  }};
  const video::VideoQuadTint tint{-0.5F, 1.25F, 2.0F, 0.4F};

  const auto layout = video::makeEmbeddedYuvQuadLayout(points, points, tint);
  expect(layout.has_value(), "unclamped finite tint is accepted");
  if (!layout) {
    return;
  }

  for (const auto &vertex : layout->vertices) {
    expect(vertex.r == -0.5F && vertex.g == 1.25F && vertex.b == 2.0F &&
               vertex.a == 0.4F,
           "the exact tint is replicated to every vertex without clamping");
  }
  expect(layout->indices == std::array<std::uint16_t, 6>{0, 1, 2, 0, 2, 3},
         "quad uses the fixed BL/BR/TR then BL/TR/TL winding");
}

void testEmbeddedYuvQuadRejectsNonFiniteInputs() {
  const std::array<video::VideoQuadPoint, 4> finitePoints = {{
      {0.0F, 1.0F},
      {1.0F, 1.0F},
      {1.0F, 0.0F},
      {0.0F, 0.0F},
  }};
  const video::VideoQuadTint finiteTint{0.0F, 1.0F, -3.0F, 2.0F};
  auto invalidDestinations = finitePoints;
  invalidDestinations[1].y = std::numeric_limits<float>::infinity();
  auto invalidUvs = finitePoints;
  invalidUvs[2].x = std::numeric_limits<float>::quiet_NaN();
  const video::VideoQuadTint invalidTint{
      0.0F, -std::numeric_limits<float>::infinity(), 1.0F, 1.0F};

  expect(!video::makeEmbeddedYuvQuadLayout(invalidDestinations, finitePoints,
                                           finiteTint),
         "non-finite destination geometry is rejected");
  expect(!video::makeEmbeddedYuvQuadLayout(finitePoints, invalidUvs,
                                           finiteTint),
         "non-finite UV geometry is rejected");
  expect(!video::makeEmbeddedYuvQuadLayout(finitePoints, finitePoints,
                                           invalidTint),
         "non-finite tint is rejected");
}

} // namespace

int main() {
  testRejectsInvalidAndUnrepresentableDimensions();
  testOnePixelFrameKeepsChromaPlanes();
  testEvenAndOddDimensionsUseCeilingChromaDivision();
  testPaddedPitchesAreCheckedAndAccounted();
  testEmbeddedYuvQuadPreservesArbitraryDestinationPoints();
  testEmbeddedYuvQuadPreservesTrimmedUvs();
  testEmbeddedYuvQuadReplicatesTintAndUsesFixedWinding();
  testEmbeddedYuvQuadRejectsNonFiniteInputs();
  if (failures != 0) {
    std::cerr << failures << " video frame layout test(s) failed\n";
    return 1;
  }
  std::cout << "Video frame layout tests passed\n";
  return 0;
}
