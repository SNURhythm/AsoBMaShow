#include "skin/beatoraja/MusicSelectSkinModelResolver.h"

#include "music_select_runtime_ledger_assertions.h"

#include <iostream>
#include <string_view>

namespace {

using namespace skin;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinDestinationBody destinationAt(int x) {
  return {.frames = {{.timeMillis = 0,
                      .x = static_cast<double>(x),
                      .width = 100,
                      .height = 20}}};
}

SkinObjectDefinition object(SkinObjectId id, std::string name,
                            SkinObjectPayload payload) {
  return {.id = id, .authoredName = std::move(name),
          .payload = std::move(payload)};
}

void testPinnedSongListResolutionUsesTypeSpecificFirstMatches() {
  BeatorajaSkinModel model;
  model.songListDefinition = SkinSongListDefinition{
      .id = "song-list",
      .center = 100000,
      .clickable = {-1000, 0, 1000},
      .listOff = {{.objectName = "ignored-off-name",
                   .destination = destinationAt(10)}},
      .listOn = {{.objectName = "bar-images",
                  .destination = destinationAt(20)}},
      .text = {{.objectName = "title", .destination = destinationAt(30)}},
      .level = {{.objectName = "level", .destination = destinationAt(40)}},
      .lamp = {{.objectName = "lamp", .destination = destinationAt(50)}},
      .graph = SkinSongListDestinationDefinition{
          .objectName = "distribution", .destination = destinationAt(60)},
  };

  SkinImageObject ordinaryImage;
  ordinaryImage.definitionKind = SkinImageDefinitionKind::Image;
  SkinImageObject imageSet;
  imageSet.definitionKind = SkinImageDefinitionKind::ImageSet;
  imageSet.orderedStates = {{.resource = 11}, {.resource = 12}};
  SkinImageObject lamp;
  lamp.definitionKind = SkinImageDefinitionKind::Image;
  SkinSelectDistributionGraphObject distribution{
      .type = SkinSelectDistributionGraphType::Normal,
      .sprite = {.resource = 13}};

  model.objects = {
      object(1, "bar-images", ordinaryImage),
      object(2, "bar-images", imageSet),
      object(3, "title", SkinTextObject{.literal = "first"}),
      object(4, "title", SkinTextObject{.literal = "second"}),
      object(5, "level", SkinNumberObject{.digitCount = 2}),
      object(6, "lamp", lamp),
      object(7, "distribution", distribution),
  };

  const auto resolved = MusicSelectSkinModelResolver{}.resolve(model);
  require(resolved.songList.has_value(), "song list resolves");
  if (!resolved.songList) {
    return;
  }
  const auto &songList = *resolved.songList;
  require(songList.center == 100000 && songList.clickable == model.songListDefinition->clickable,
          "authored center and clickable values are retained");
  require(songList.listOn.size() == 1 && songList.listOn[0].object == 2 &&
              songList.listOff.size() == 1 && songList.listOff[0].object == 2,
          "list-on ImageSet drives both paired bar images");
  require(songList.text.size() == 1 && songList.text[0].object == 3,
          "first matching Text definition wins");
  require(songList.level.size() == 1 && songList.level[0].object == 5 &&
              songList.lamp.size() == 1 && songList.lamp[0].object == 6,
          "Value and Image fields resolve only their pinned categories");
  require(songList.graph && songList.graph->object == 7,
          "negative Graph resolves as the select distribution graph");
}

void testMissingAndExcessEntriesRemainRepresented() {
  BeatorajaSkinModel model;
  SkinSongListDefinition definition{.id = "song-list"};
  for (int index = 0; index < 61; ++index) {
    definition.listOn.push_back(
        {.objectName = "missing", .destination = destinationAt(index)});
    definition.listOff.push_back(
        {.objectName = "also-ignored", .destination = destinationAt(-index)});
  }
  definition.text.push_back(
      {.objectName = "wrong-kind", .destination = destinationAt(70)});
  definition.graph = SkinSongListDestinationDefinition{
      .objectName = "positive-graph", .destination = destinationAt(80)};
  model.songListDefinition = std::move(definition);

  SkinImageObject wrongKind;
  wrongKind.definitionKind = SkinImageDefinitionKind::Image;
  model.objects.push_back(object(1, "wrong-kind", wrongKind));
  model.objects.push_back(
      object(2, "positive-graph", SkinGraphObject{}));

  const auto resolved = MusicSelectSkinModelResolver{}.resolve(model);
  require(resolved.songList && resolved.songList->listOn.size() == 61 &&
              resolved.songList->listOff.size() == 61,
          "entries outside fixed SkinBar slots remain in authored order");
  require(resolved.songList && resolved.songList->listOn.back().object == 0 &&
              resolved.songList->text.front().object == 0,
          "missing or wrong-category definitions remain unresolved");
  require(resolved.songList && resolved.songList->graph &&
              resolved.songList->graph->object == 0,
          "a non-negative generic Graph is not a select distribution graph");
}

void testEveryNegativeGraphOtherThanMinusOneKeepsJudgeShape() {
  BeatorajaSkinModel model;
  model.songListDefinition = SkinSongListDefinition{
      .id = "song-list",
      .graph = SkinSongListDestinationDefinition{
          .objectName = "judge-distribution",
          .destination = destinationAt(90)},
  };
  model.objects.push_back(object(
      9, "judge-distribution",
      SkinSelectDistributionGraphObject{
          .type = SkinSelectDistributionGraphType::Judge}));

  const auto resolved = MusicSelectSkinModelResolver{}.resolve(model);
  require(resolved.songList && resolved.songList->graph &&
              resolved.songList->graph->object == 9 &&
              std::get<SkinSelectDistributionGraphObject>(
                  model.objects.front().payload)
                      .type == SkinSelectDistributionGraphType::Judge,
          "all negative Graph types other than -1 use the judge shape");
}

} // namespace

int main(int argc, char **argv) {
  testPinnedSongListResolutionUsesTypeSpecificFirstMatches();
  testMissingAndExcessEntriesRemainRepresented();
  testEveryNegativeGraphOtherThanMinusOneKeepsJudgeShape();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_skin_model_tests", failures,
      "music-select skin model test(s) failed",
      "music-select skin model tests passed");
}
