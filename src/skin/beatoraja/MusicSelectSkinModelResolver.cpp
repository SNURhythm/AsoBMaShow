#include "MusicSelectSkinModelResolver.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace skin {
namespace {

template <typename Payload>
SkinObjectId firstObject(std::span<const SkinObjectDefinition> objects,
                         std::string_view name) {
  const auto found = std::ranges::find_if(objects, [&](const auto &object) {
    return object.authoredName == name &&
           std::holds_alternative<Payload>(object.payload);
  });
  return found != objects.end() ? found->id : SkinObjectId{0};
}

SkinObjectId firstImage(
    std::span<const SkinObjectDefinition> objects, std::string_view name,
    SkinImageDefinitionKind kind) {
  const auto found = std::ranges::find_if(objects, [&](const auto &object) {
    const auto *image = std::get_if<SkinImageObject>(&object.payload);
    return object.authoredName == name && image &&
           image->definitionKind == kind;
  });
  return found != objects.end() ? found->id : SkinObjectId{0};
}

SkinSongListPresentation
presentation(const SkinSongListDestinationDefinition &definition,
             SkinObjectId object) {
  return {.object = object,
          .destination = definition.destination,
          .source = definition.source};
}

template <typename Payload>
std::vector<SkinSongListPresentation>
resolveTyped(std::span<const SkinSongListDestinationDefinition> definitions,
             std::span<const SkinObjectDefinition> objects) {
  std::vector<SkinSongListPresentation> result;
  result.reserve(definitions.size());
  for (const auto &definition : definitions) {
    result.push_back(
        presentation(definition,
                     firstObject<Payload>(objects, definition.objectName)));
  }
  return result;
}

std::vector<SkinSongListPresentation>
resolveImages(std::span<const SkinSongListDestinationDefinition> definitions,
              std::span<const SkinObjectDefinition> objects) {
  std::vector<SkinSongListPresentation> result;
  result.reserve(definitions.size());
  for (const auto &definition : definitions) {
    result.push_back(presentation(
        definition, firstImage(objects, definition.objectName,
                               SkinImageDefinitionKind::Image)));
  }
  return result;
}

} // namespace

MusicSelectSkinModelResolution
MusicSelectSkinModelResolver::resolve(const BeatorajaSkinModel &model) const {
  if (!model.songListDefinition) {
    return {};
  }

  const auto &definition = *model.songListDefinition;
  const std::span<const SkinObjectDefinition> objects(model.objects);
  SkinSongListObject songList{
      .center = definition.center,
      .clickable = definition.clickable,
      .text = resolveTyped<SkinTextObject>(definition.text, objects),
      .level = resolveTyped<SkinNumberObject>(definition.level, objects),
      .lamp = resolveImages(definition.lamp, objects),
      .playerLamp = resolveImages(definition.playerLamp, objects),
      .rivalLamp = resolveImages(definition.rivalLamp, objects),
      .trophy = resolveImages(definition.trophy, objects),
      .label = resolveImages(definition.label, objects),
  };

  songList.listOn.reserve(definition.listOn.size());
  for (const auto &on : definition.listOn) {
    songList.listOn.push_back(presentation(
        on, firstImage(objects, on.objectName,
                       SkinImageDefinitionKind::ImageSet)));
  }
  songList.listOff.reserve(definition.listOff.size());
  for (std::size_t index = 0; index < definition.listOff.size(); ++index) {
    const SkinObjectId object =
        index < songList.listOn.size() ? songList.listOn[index].object : 0;
    songList.listOff.push_back(presentation(definition.listOff[index], object));
  }

  if (definition.graph) {
    songList.graph = presentation(
        *definition.graph,
        firstObject<SkinSelectDistributionGraphObject>(
            objects, definition.graph->objectName));
  }
  return {.songList = std::move(songList)};
}

} // namespace skin
