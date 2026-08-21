#include "skin/beatoraja/JsonGameplaySkinDecoder.h"

#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/SkinModelValidator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace skin;
using Json = nlohmann::json;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<std::byte> readFixture(std::string_view name) {
  const fs::path path = fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/json" / name;
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "JSON gameplay fixture opens");
  const std::string text{std::istreambuf_iterator<char>(input), {}};
  const auto bytes = std::as_bytes(std::span(text));
  return {bytes.begin(), bytes.end()};
}

SkinEntryId fixtureEntry(std::string_view name) {
  return {.package = {.directoryName = "JsonContract",
                      .collisionKey = "jsoncontract"},
          .packageRelativePath = "skin/" + std::string(name),
          .collisionKey = "skin/" + std::string(name)};
}

JsonGameplaySkinDecodeResult
decodeFixture(std::string_view name,
              const EntryProfileSettings *desired = nullptr) {
  const auto bytes = readFixture(name);
  return JsonGameplaySkinDecoder{}.decode(bytes, fixtureEntry(name), desired,
                                          gameplaySkinBuiltinCatalog());
}

JsonGameplaySkinDecodeResult decodeInline(std::string_view text) {
  return JsonGameplaySkinDecoder{}.decode(
      std::as_bytes(std::span(text)), fixtureEntry("inline.json"), nullptr,
      gameplaySkinBuiltinCatalog());
}

const SkinObjectDefinition *findObject(const BeatorajaSkinModel &model,
                                       std::string_view name) {
  const auto found = std::ranges::find_if(
      model.objects,
      [&](const SkinObjectDefinition &object) {
        return object.authoredName == name;
      });
  return found == model.objects.end() ? nullptr : &*found;
}

const SkinDestination *findDestination(const BeatorajaSkinModel &model,
                                       SkinObjectId id) {
  const auto found = std::ranges::find_if(
      model.destinations,
      [&](const SkinDestination &destination) {
        return destination.object == id;
      });
  return found == model.destinations.end() ? nullptr : &*found;
}

template <typename Payload>
const Payload *payload(const BeatorajaSkinModel &model, std::string_view name) {
  const auto *object = findObject(model, name);
  return object == nullptr ? nullptr : std::get_if<Payload>(&object->payload);
}

bool hasDiagnostic(const JsonGameplaySkinDecodeResult &decoded,
                   std::string_view code) {
  return std::ranges::any_of(decoded.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.code == code;
  });
}

struct CancellationCheckpoint {
  std::stop_source *source = nullptr;
  StaticSkinDecodePhase phase = StaticSkinDecodePhase::JsonModel;
  std::size_t stopAt = 0;
};

void requestCancellationAtCheckpoint(StaticSkinDecodePhase phase,
                                     std::size_t workItem,
                                     void *context) noexcept {
  auto &checkpoint = *static_cast<CancellationCheckpoint *>(context);
  if (phase == checkpoint.phase && workItem == checkpoint.stopAt) {
    checkpoint.source->request_stop();
  }
}

Json parseJsonFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "JSON contract file opens");
  return input.good() ? Json::parse(input, nullptr, true, true)
                      : Json::object();
}

void testAllFieldFixtureCoversThePinnedJsonLedger() {
  const fs::path root = ASOBMASHOW_SOURCE_DIR;
  const Json fixture = parseJsonFile(
      root / "tests/fixtures/beatoraja_skin/json/all_gameplay_fields.json");
  const Json surface = parseJsonFile(
      root / "docs/skin-compat/beatoraja-gameplay-source-surface-v1.json");
  std::map<std::string, std::set<std::string>> observed;
  const auto addObject = [&](std::string_view className, const Json &object) {
    if (!object.is_object()) return;
    for (auto field = object.begin(); field != object.end(); ++field) {
      observed[std::string(className)].insert(field.key());
    }
  };
  const auto addArray = [&](std::string_view className,
                            std::string_view fieldName) {
    const auto found = fixture.find(std::string(fieldName));
    if (found == fixture.end() || !found->is_array()) return;
    for (const auto &item : *found) addObject(className, item);
  };
  addObject("JsonSkin.Skin", fixture);
  const std::array arrays{
      std::pair{"JsonSkin.BPMGraph", "bpmgraph"},
      std::pair{"JsonSkin.FloatValue", "floatvalue"},
      std::pair{"JsonSkin.Font", "font"},
      std::pair{"JsonSkin.GaugeGraph", "gaugegraph"},
      std::pair{"JsonSkin.Graph", "graph"},
      std::pair{"JsonSkin.HiddenCover", "hiddenCover"},
      std::pair{"JsonSkin.HitErrorVisualizer", "hiterrorvisualizer"},
      std::pair{"JsonSkin.Image", "image"},
      std::pair{"JsonSkin.ImageSet", "imageset"},
      std::pair{"JsonSkin.Judge", "judge"},
      std::pair{"JsonSkin.JudgeGraph", "judgegraph"},
      std::pair{"JsonSkin.LiftCover", "liftCover"},
      std::pair{"JsonSkin.PMchara", "pmchara"},
      std::pair{"JsonSkin.Slider", "slider"},
      std::pair{"JsonSkin.Text", "text"},
      std::pair{"JsonSkin.TimingDistributionGraph",
                "timingdistributiongraph"},
      std::pair{"JsonSkin.TimingVisualizer", "timingvisualizer"},
      std::pair{"JsonSkin.Value", "value"}};
  for (const auto &[className, fieldName] : arrays) {
    addArray(className, fieldName);
  }
  for (const auto &font : fixture.at("font")) {
    for (const auto &fallback : font.at("fallback")) {
      addObject("JsonSkin.FontFallback", fallback);
      if (fallback.is_string()) {
        observed["JsonSkin.FontFallback"].insert("path");
        observed["JsonSkin.FontFallback"].insert("type");
      }
    }
  }
  std::vector<const Json *> destinations;
  for (const auto &destination : fixture.at("destination")) {
    destinations.push_back(&destination);
  }
  for (const auto field : {"group", "bpm", "stop", "time"}) {
    for (const auto &destination : fixture.at("note").at(field)) {
      destinations.push_back(&destination);
    }
  }
  for (const auto &judge : fixture.at("judge")) {
    for (const auto field : {"images", "numbers"}) {
      for (const auto &destination : judge.at(field)) {
        destinations.push_back(&destination);
      }
    }
  }
  bool numericOption = false;
  bool propertyOption = false;
  for (const Json *destination : destinations) {
    addObject("JsonSkin.Destination", *destination);
    if (const auto rect = destination->find("mouseRect");
        rect != destination->end()) {
      addObject("JsonSkin.Rect", *rect);
    }
    if (const auto frames = destination->find("dst");
        frames != destination->end() && frames->is_array()) {
      for (const auto &frame : *frames) {
        addObject("JsonSkin.Animation", frame);
      }
    }
    if (const auto options = destination->find("op");
        options != destination->end() && options->is_array()) {
      numericOption = numericOption ||
                      std::ranges::any_of(*options, [](const Json &option) {
                        return option.is_number();
                      });
      propertyOption = propertyOption ||
                       std::ranges::any_of(*options, [](const Json &option) {
                         return option.is_string();
                       });
    }
  }
  for (const auto &frame : fixture.at("note").at("dst")) {
    addObject("JsonSkin.Animation", frame);
  }
  if (numericOption) observed["JsonSkin.DestinationOption"].insert("id");
  if (propertyOption)
    observed["JsonSkin.DestinationOption"].insert("property");

  std::size_t required = 0;
  std::size_t covered = 0;
  for (const auto &feature : surface.at("features")) {
    const std::string id = feature.at("id").get<std::string>();
    if (!id.starts_with("json.field.")) continue;
    ++required;
    const std::string symbol =
        feature.at("source").at("symbol").get<std::string>();
    const std::size_t separator = symbol.rfind('.');
    const std::string className = symbol.substr(0, separator);
    const std::string fieldName = symbol.substr(separator + 1);
    if (observed[className].contains(fieldName)) ++covered;
  }
  expect(required == 308 && covered == required,
         "all_gameplay_fields.json covers all 308 pinned JSON ledger fields");
}

template <typename Binding>
bool isStaticBinding(const Binding &binding) {
  return std::holds_alternative<SkinBuiltinPropertySelector>(binding.source);
}

void testPinnedDefaultsProduceTypedStaticModel() {
  const auto decoded = decodeFixture("defaults.json");
  expect(decoded.header.has_value() && decoded.configuration.has_value() &&
             decoded.reconciledSettings.has_value() &&
             decoded.model.has_value(),
         "default JSON document produces the complete value-owned result");
  if (!decoded.header || !decoded.configuration || !decoded.model) {
    return;
  }

  const auto &header = *decoded.header;
  expect(header.type == 0 && header.width == 1280 && header.height == 720 &&
             header.name.empty() && header.author.empty(),
         "omitted JsonSkin.Skin header fields use pinned Java defaults");
  expect(header.categories.empty() && header.options.empty() &&
             header.files.empty() && header.offsets.size() == 4,
         "empty header arrays and four gameplay offsets match JSONSkinLoader");

  const auto &timing = decoded.model->timing;
  expect(timing.fadeoutMillis == 0 && timing.inputMillis == 0 &&
             timing.sceneMillis == 0 && timing.closeMillis == 0 &&
             timing.loadEndMillis == 0 && timing.playStartMillis == 0 &&
             timing.judgeTimerMillis == 1 && timing.finishMarginMillis == 0,
         "omitted gameplay timing fields match JsonSkin.Skin defaults");

  const auto *image = payload<SkinImageObject>(*decoded.model, "image-defaults");
  expect(image != nullptr && image->orderedStates.size() == 1 &&
             image->orderedStates.front().frames.size() == 1 &&
             image->orderedStates.front().frames.front().gridColumns == 1 &&
             image->orderedStates.front().frames.front().gridRows == 1 &&
             image->clickMode == 0,
         "omitted Image primitives retain divx/divy one and click zero");

  const auto *gaugeGraph =
      payload<SkinGaugeGraphObject>(*decoded.model, "gaugegraph-defaults");
  const auto *judgeGraph = payload<SkinNoteDistributionGraphObject>(
      *decoded.model, "judgegraph-defaults");
  const auto *bpm =
      payload<SkinBpmGraphObject>(*decoded.model, "bpmgraph-defaults");
  const auto *hit = payload<SkinHitErrorVisualizerObject>(
      *decoded.model, "hiterror-defaults");
  const auto *timingVisualizer = payload<SkinTimingVisualizerObject>(
      *decoded.model, "timing-defaults");
  const auto *timingDistribution = payload<SkinTimingDistributionGraphObject>(
      *decoded.model, "timing-distribution-defaults");
  expect(gaugeGraph != nullptr &&
             gaugeGraph->rgba[0] ==
                 std::array<std::uint32_t, 4>{0xff0000ffU, 0x440000ffU,
                                               0xff00ffffU, 0x440044ffU},
         "GaugeGraph legacy colors match pinned defaults");
  expect(judgeGraph != nullptr &&
             judgeGraph->type == SkinNoteDistributionGraphType::Normal &&
             judgeGraph->delayMillis == 500 &&
             !judgeGraph->backgroundTextureOff && !judgeGraph->reverseOrder &&
             !judgeGraph->noGap && !judgeGraph->noHorizontalGap,
         "JudgeGraph defaults match JsonSkin.JudgeGraph");
  expect(bpm != nullptr && bpm->delayMillis == 0 && bpm->lineWidth == 2 &&
             bpm->mainRgba == 0x00ff00ffU &&
             bpm->transitionRgba == 0x7f7f7fffU,
         "BPMGraph defaults and RGB opacity match its pinned constructor");
  expect(hit != nullptr && hit->width == 301 &&
             hit->judgeWidthMillis == 150 && hit->lineWidth == 1 &&
             hit->colorMode && hit->hitErrorMode && hit->emaMode == 1 &&
             hit->alpha == 0.1F && hit->windowLength == 30 &&
             !hit->transparent && hit->drawDecay,
         "HitErrorVisualizer defaults match JsonSkin.java");
  expect(timingVisualizer != nullptr && timingVisualizer->width == 301 &&
             timingVisualizer->judgeWidthMillis == 150 &&
             timingVisualizer->lineWidth == 1 &&
             !timingVisualizer->transparent && timingVisualizer->drawDecay,
         "TimingVisualizer defaults match JsonSkin.java");
  expect(timingDistribution != nullptr &&
             timingDistribution->width == 301 &&
             timingDistribution->lineWidth == 1 &&
             timingDistribution->drawAverage && timingDistribution->drawDev,
         "TimingDistributionGraph defaults remain a typed gameplay no-op");

  const auto *hidden =
      payload<SkinCoverObject>(*decoded.model, "hidden-defaults");
  const auto *lift = payload<SkinCoverObject>(*decoded.model, "lift-defaults");
  const auto *practice =
      payload<SkinPracticeObject>(*decoded.model, "practice-defaults");
  expect(hidden != nullptr && hidden->kind == SkinCoverKind::Hidden &&
             hidden->disappearLine == -1.0 &&
             hidden->disappearLineLinksLift,
         "HiddenCover defaults match JsonSkin.HiddenCover");
  expect(lift != nullptr && lift->kind == SkinCoverKind::Lift &&
             lift->disappearLine == -1.0 &&
             !lift->disappearLineLinksLift,
         "LiftCover defaults match JsonSkin.LiftCover");
  expect(practice != nullptr && practice->visibleItems == 10,
         "Practice visibleItems defaults to ten");
  expect(findObject(*decoded.model, "pmchara-defaults") == nullptr,
         "PMchara Integer.MIN_VALUE type follows the pinned unresolved path");

  const auto *first = findObject(*decoded.model, "image-defaults");
  const auto *firstDestination =
      first == nullptr ? nullptr : findDestination(*decoded.model, first->id);
  expect(firstDestination != nullptr &&
             firstDestination->presentation.loop == 0 &&
             firstDestination->presentation.stretch == SkinStretchMode::Stretch &&
             firstDestination->presentation.frames.size() == 1 &&
             firstDestination->presentation.frames.front().timeMillis == 0 &&
             firstDestination->presentation.frames.front().rgba ==
                 std::array<std::uint8_t, 4>{255, 255, 255, 255},
         "Destination and first Animation omitted primitives use pinned defaults");

  expect(std::ranges::none_of(decoded.model->objects, [](const auto &object) {
           return std::holds_alternative<SkinBlankObject>(object.payload);
         }),
         "valid default constructs never become SkinBlankObject");
}

void testAllGameplayFieldsPreserveOrderProvenanceAndPayloads() {
  const auto declarationDefaults =
      decodeFixture("all_gameplay_fields.json");
  expect(declarationDefaults.configuration &&
             declarationDefaults.reconciledSettings &&
             declarationDefaults.configuration->options.contains(
                 "Lane style") &&
             declarationDefaults.configuration->options.at("Lane style") ==
                 902 &&
             declarationDefaults.configuration->filePaths.contains(
                 "Atlas choice") &&
             declarationDefaults.configuration->filePaths.at(
                 "Atlas choice") == "default" &&
             declarationDefaults.configuration->offsets.contains(
                 "Custom move") &&
             declarationDefaults.configuration->offsets.at("Custom move") ==
                 ConfigOffset{},
         "authored option, file, and offset defaults reconcile without profile state");

  EntryProfileSettings desired;
  desired.options["Lane style"] = 901;
  desired.filePaths["Atlas choice"] = "custom.png";
  desired.offsets["Custom move"] =
      {.x = 1, .y = 2, .w = 3, .h = 4, .r = 5, .a = 6};
  desired.viewport = {.mode = ViewportMode::Custom,
                      .customBase = CustomViewportBase::Stretch,
                      .scaleX = 1.25F,
                      .scaleY = 1.5F,
                      .translateX = 8.0F,
                      .translateY = 9.0F};
  const auto decoded = decodeFixture("all_gameplay_fields.json", &desired);
  expect(decoded.header && decoded.configuration &&
             decoded.reconciledSettings && decoded.model,
         "all-field JSON fixture decodes completely");
  expect(!hasDiagnostic(decoded, "skin_json_field_unclassified"),
         "all gameplay ledger fields are classified by the JSON decoder");
  if (!decoded.header || !decoded.configuration ||
      !decoded.reconciledSettings || !decoded.model) {
    return;
  }

  expect(decoded.header->name == "JSON gameplay field contract" &&
             decoded.header->author == "AsoBMaShow tests" &&
             decoded.header->categories.size() == 1 &&
             decoded.header->options.size() == 1 &&
             decoded.header->files.size() == 1 &&
             decoded.header->offsets.size() == 5,
         "header, customization declarations, and synthesized offsets decode");
  expect(decoded.configuration->orderedOptions.size() == 1 &&
             decoded.configuration->orderedOptions.front().value == 901 &&
             decoded.configuration->enabledOptionIds.contains(901) &&
             decoded.configuration->filePaths.at("Atlas choice") ==
                 "custom.png" &&
             decoded.configuration->offsets.at("Custom move") ==
                 ConfigOffset{.x = 1, .y = 0, .w = 3, .h = 0, .r = 5, .a = 0},
         "desired configuration is reconciled by name and offset permissions");
  expect(decoded.reconciledSettings->viewport == desired.viewport &&
             decoded.configuration->lowercaseSha256 ==
                 skinConfigurationDigest(*decoded.reconciledSettings),
         "reconciled settings retain viewport and agree with the digest");

  const auto &model = *decoded.model;
  expect(model.timing.fadeoutMillis == 901 && model.timing.inputMillis == 902 &&
             model.timing.sceneMillis == 903 && model.timing.closeMillis == 904 &&
             model.timing.loadEndMillis == 905 &&
             model.timing.playStartMillis == 906 &&
             model.timing.judgeTimerMillis == 907 &&
             model.timing.finishMarginMillis == 908,
         "every authored gameplay timing field is retained");

  const std::array<std::string_view, 23> expectedOrder{
      "image-all", "imageset-all", "value-all", "float-all", "text-all",
      "slider-all", "graph-all", "gaugegraph-all", "judgegraph-all",
      "bpmgraph-all", "hiterror-all", "timing-all",
      "timing-distribution-all", "gauge-all", "note-all", "hidden-all",
      "lift-all", "practice-all", "bga-all", "judge-all", "pmchara-all",
      "collision", "-10"};
  expect(model.destinations.size() == expectedOrder.size(),
         "one canonical destination is retained for every resolved authored entry");
  for (std::size_t index = 0;
       index < model.destinations.size() && index < expectedOrder.size(); ++index) {
    const auto found = std::ranges::find_if(
        model.objects, [&](const SkinObjectDefinition &object) {
          return object.id == model.destinations[index].object;
        });
    expect(found != model.objects.end() &&
               found->authoredName == expectedOrder[index] &&
               model.destinations[index].presentation.authoredOrdinal == index,
           "destination and object order follow the authored destination array");
    expect(model.destinations[index].source.virtualPath ==
               "skin/all_gameplay_fields.json" &&
               model.destinations[index].source.line > 0 &&
               model.destinations[index].source.column > 0,
           "each destination retains entry-relative JSON source location");
  }
  expect(!model.destinations.empty() && model.destinations.front().source.line == 445 &&
             model.destinations.front().source.column == 5,
         "the first destination has its exact authored line and column");

  const auto *image = payload<SkinImageObject>(model, "image-all");
  const auto *number = payload<SkinNumberObject>(model, "value-all");
  const auto *floating = payload<SkinFloatObject>(model, "float-all");
  const auto *text = payload<SkinTextObject>(model, "text-all");
  const auto *slider = payload<SkinSliderObject>(model, "slider-all");
  const auto *graph = payload<SkinGraphObject>(model, "graph-all");
  expect(image != nullptr && image->orderedStates.size() == 2 &&
             image->orderedStates[0].frames.size() == 6 &&
             image->clickMode == 2 && image->clickEvent.has_value(),
         "Image source rectangle, divisions, len, timer, ref, act, and click decode");
  expect(number != nullptr && number->digitCount == 6 &&
             number->spacing == 3 && number->alignment == 2 &&
             number->zeroPadding == SkinZeroPaddingMode::Zero &&
             number->perDigitOffsets.size() == 1,
         "Value fields and nested digit offsets decode to SkinNumberObject");
  expect(floating != nullptr && floating->integerDigits == 2 &&
             floating->fractionalDigits == 3 && floating->spacing == 4 &&
             floating->alignment == 1 && floating->signVisible &&
             floating->gain == 1.5 && floating->perDigitOffsets.size() == 1,
         "FloatValue fields decode to the typed float payload");
  expect(text != nullptr && text->pointSize == 24 && text->alignment == 1 &&
             text->literal == "constant" && text->editable && text->wrapping &&
             text->overflow == 2 && text->outlineRgba ==
                 std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44} &&
             text->outlineWidth == 1.25 && text->shadowRgba ==
                 std::array<std::uint8_t, 4>{0x55, 0x66, 0x77, 0x88} &&
             text->shadowOffsetX == 2.5 && text->shadowOffsetY == 3.5 &&
             text->shadowSmoothness == 0.75,
         "Text style, literal, property, writer, and editability fields decode");
  expect(slider != nullptr && slider->direction == 3 && slider->range == 120.0 &&
             !slider->changeable && slider->writer.has_value(),
         "Slider sprite, value, writer, direction, range, and flags decode");
  expect(graph != nullptr && graph->direction == 2,
         "Graph sprite, value, direction, and range-source fields decode");

  const auto *gaugeGraph = payload<SkinGaugeGraphObject>(model, "gaugegraph-all");
  const auto *judgeGraph =
      payload<SkinNoteDistributionGraphObject>(model, "judgegraph-all");
  const auto *bpm = payload<SkinBpmGraphObject>(model, "bpmgraph-all");
  const auto *hit =
      payload<SkinHitErrorVisualizerObject>(model, "hiterror-all");
  const auto *timing =
      payload<SkinTimingVisualizerObject>(model, "timing-all");
  const auto *distribution = payload<SkinTimingDistributionGraphObject>(
      model, "timing-distribution-all");
  expect(gaugeGraph != nullptr && gaugeGraph->rgba.front().front() == 0x01020304U &&
             gaugeGraph->rgba.back().back() == 0x8090a0b0U,
         "GaugeGraph direct 24-color ordering is preserved");
  expect(judgeGraph != nullptr &&
             judgeGraph->type == SkinNoteDistributionGraphType::EarlyLate &&
             judgeGraph->backgroundTextureOff &&
             judgeGraph->delayMillis == 750 && judgeGraph->reverseOrder &&
             judgeGraph->noGap && judgeGraph->noHorizontalGap,
         "JudgeGraph fields decode without a blank placeholder");
  expect(bpm != nullptr && bpm->delayMillis == 30 && bpm->lineWidth == 3 &&
             bpm->mainRgba == 0x010203ffU &&
             bpm->transitionRgba == 0x515253ffU,
         "BPMGraph fields preserve RGB source colors");
  expect(hit != nullptr && hit->width == 401 && hit->judgeWidthMillis == 160 &&
             hit->lineWidth == 3 && !hit->colorMode && !hit->hitErrorMode &&
             hit->emaMode == 2 && hit->alpha == 0.25F &&
             hit->windowLength == 40 && !hit->drawDecay,
         "HitErrorVisualizer fields decode to its complete typed payload");
  expect(timing != nullptr && timing->width == 402 &&
             timing->judgeWidthMillis == 170 && timing->lineWidth == 4 &&
             !timing->drawDecay,
         "TimingVisualizer fields decode to its complete typed payload");
  expect(distribution != nullptr && distribution->width == 403 &&
             distribution->lineWidth == 3 && !distribution->drawAverage &&
             !distribution->drawDev,
         "TimingDistributionGraph remains typed with every authored field");

  const auto *gauge = payload<SkinGaugeObject>(model, "gauge-all");
  const auto *note = payload<SkinNoteObject>(model, "note-all");
  const auto *hidden = payload<SkinCoverObject>(model, "hidden-all");
  const auto *lift = payload<SkinCoverObject>(model, "lift-all");
  const auto *practice = payload<SkinPracticeObject>(model, "practice-all");
  const auto *bga = payload<SkinBgaObject>(model, "bga-all");
  const auto *judge = payload<SkinJudgeObject>(model, "judge-all");
  const auto *pm = payload<SkinPmCharaObject>(model, "pmchara-all");
  expect(gauge != nullptr && gauge->orderedNodes.size() == 36 &&
             gauge->parts == 60 &&
             gauge->animation == SkinGaugeAnimationType::Decrease &&
             gauge->animationRange == 4 && gauge->animationCycleMillis == 44 &&
             gauge->resultStartMillis == 100 && gauge->resultEndMillis == 600,
         "Gauge nodes and every animation field decode");
  expect(note != nullptr && note->lanes.size() == 1 &&
             note->lanes.front().authoredNoteHeight == 11.5 &&
             !note->lanes.front().secondaryDestinationY.has_value() &&
             note->expansionRatePercent == std::array<int, 2>{110, 120} &&
             note->lines.size() == 4,
         "NoteSet visual arrays, MIN sentinel, geometry, and line destinations decode");
  expect(hidden != nullptr && hidden->kind == SkinCoverKind::Hidden &&
             hidden->disappearLine == 300.0 &&
             !hidden->disappearLineLinksLift,
         "HiddenCover fields decode to a typed cover");
  expect(lift != nullptr && lift->kind == SkinCoverKind::Lift &&
             lift->disappearLine == 301.0 && lift->disappearLineLinksLift,
         "LiftCover fields decode to a typed cover");
  expect(practice != nullptr && practice->visibleItems == 12 && bga != nullptr,
         "Practice and BGA identities resolve to typed gameplay objects");
  expect(judge != nullptr && judge->player == 1 &&
             judge->shiftImageByHalfDetailWidth &&
             !judge->grades.empty() && judge->grades.front().image.has_value() &&
             judge->grades.front().detailNumber.has_value(),
         "Judge nested images/numbers and presentation fields decode");
  expect(pm != nullptr && pm->sourceName == "character" &&
             pm->sourcePath == "assets/character.chp" && pm->color == 2 &&
             pm->type == 7 && pm->side == 2,
         "PMchara source, color, type, and side decode");

  const auto *collision = findObject(model, "collision");
  const auto *builtin = payload<SkinBuiltinImageObject>(model, "-10");
  expect(collision != nullptr &&
             std::holds_alternative<SkinImageObject>(collision->payload),
         "object ID collision uses pinned Image-before-JudgeGraph precedence");
  expect(builtin != nullptr && builtin->referenceId == 10,
         "negative numeric destination IDs resolve before authored objects");

  const auto *imageDefinition = findObject(model, "image-all");
  const auto *destination = imageDefinition == nullptr
                                ? nullptr
                                : findDestination(model, imageDefinition->id);
  expect(destination != nullptr &&
             destination->presentation.frames.size() == 3 &&
             destination->presentation.frames[0].timeMillis == 100 &&
             destination->presentation.frames[0].x == 11.0 &&
             destination->presentation.frames[1].timeMillis == 100 &&
             destination->presentation.frames[1].x == 11.0 &&
             destination->presentation.frames[1].rgba ==
                 std::array<std::uint8_t, 4>{10, 20, 30, 200} &&
             destination->presentation.frames[2].timeMillis == 200 &&
             destination->presentation.offsetIds ==
                 std::vector<int>({30, 31, 77}) &&
             destination->presentation.mouseRect.has_value() &&
             destination->presentation.mouseRect->x == 4 &&
             destination->presentation.mouseRect->y == 5 &&
             destination->presentation.mouseRect->width == 6 &&
             destination->presentation.mouseRect->height == 7,
         "Animation MIN sentinels inherit before stable time ordering");

  expect(model.customEvents.size() == 1 && model.customEvents[0].id == 1001 &&
             model.customEvents[0].action &&
             model.customEvents[0].condition.has_value() &&
             model.customEvents[0].minimumIntervalMillis == 60 &&
             model.customTimers.size() == 1 &&
             model.customTimers[0].id == 1002 &&
             model.customTimers[0].timer.has_value(),
         "custom property, event, condition, and timer bindings decode");
  expect(std::ranges::all_of(model.booleanProperties, isStaticBinding<SkinBooleanPropertyBinding>) &&
             std::ranges::all_of(model.integerProperties, isStaticBinding<SkinIntegerPropertyBinding>) &&
             std::ranges::all_of(model.floatProperties, isStaticBinding<SkinFloatPropertyBinding>) &&
             std::ranges::all_of(model.stringProperties, isStaticBinding<SkinStringPropertyBinding>) &&
             std::ranges::all_of(model.timerProperties, isStaticBinding<SkinTimerPropertyBinding>) &&
             std::ranges::all_of(model.floatWriters, isStaticBinding<SkinFloatWriterBinding>) &&
             std::ranges::all_of(model.stringWriters, isStaticBinding<SkinStringWriterBinding>) &&
             std::ranges::all_of(model.events, isStaticBinding<SkinEventBinding>),
         "JSON decoding creates built-in bindings only and no Lua callbacks");
  expect(std::ranges::none_of(model.objects, [](const auto &object) {
           return std::holds_alternative<SkinBlankObject>(object.payload);
         }),
         "all valid all-field constructs retain a typed canonical payload");

  const auto validated = SkinModelValidator{}.validate(
      model, {.builtins = gameplaySkinBuiltinCatalog(),
              .callbacks = std::nullopt});
  expect(validated.model.has_value() && !validated.criticalFailure &&
             std::ranges::none_of(validated.diagnostics,
                                  [](const SkinDiagnostic &diagnostic) {
               return diagnostic.code == "skin.model.callback_runtime_missing";
             }),
         "the all-field JSON model validates without a Lua callback runtime");
}

void testMalformedUnboundedAndCallbackJsonFailAtDecoderBoundary() {
  const auto entry = fixtureEntry("invalid.json");
  const auto decodeText = [&](std::string_view text) {
    return JsonGameplaySkinDecoder{}.decode(
        std::as_bytes(std::span(text)), entry, nullptr,
        gameplaySkinBuiltinCatalog());
  };

  const auto malformed = decodeText("{\n  \"type\": 0,\n");
  expect(!malformed.header && !malformed.configuration &&
             !malformed.reconciledSettings && !malformed.model &&
             hasDiagnostic(malformed, "skin_json_parse_failed") &&
             malformed.diagnostics.front().virtualPath == "skin/invalid.json" &&
             malformed.diagnostics.front().source.has_value(),
         "nlohmann parse errors are caught with entry provenance");

  std::vector<std::byte> oversized(
      JsonGameplaySkinDecoderPolicy::maxDocumentBytes + 1, std::byte{' '});
  const auto bounded = JsonGameplaySkinDecoder{}.decode(
      oversized, entry, nullptr, gameplaySkinBuiltinCatalog());
  expect(!bounded.model && hasDiagnostic(bounded, "skin_json_limit_exceeded"),
         "JSON input is rejected before parsing when the byte bound is exceeded");

  const std::array invalidUtf8{
      std::byte{static_cast<unsigned char>('{')},
      std::byte{static_cast<unsigned char>('"')}, std::byte{0xff},
      std::byte{static_cast<unsigned char>('"')},
      std::byte{static_cast<unsigned char>(':')},
      std::byte{static_cast<unsigned char>('0')},
      std::byte{static_cast<unsigned char>('}')},
  };
  const auto invalidEncoding = JsonGameplaySkinDecoder{}.decode(
      invalidUtf8, entry, nullptr, gameplaySkinBuiltinCatalog());
  expect(!invalidEncoding.model &&
             hasDiagnostic(invalidEncoding, "skin_json_parse_failed"),
         "invalid UTF-8 is rejected at the JSON decoder boundary");

  const auto unknown = decodeText(
      R"({"type":0,"futureGameplayField":1,"destination":[]})");
  expect(unknown.model &&
             hasDiagnostic(unknown, "skin_json_field_unclassified"),
         "unknown JSON fields remain visible as compatibility diagnostics");

  const auto scripted = decodeText(R"({
    "type": 0,
    "source": [{"id":"atlas","path":"atlas.png"}],
    "text": [{"id":"scripted","font":"missing","value":"return 1"}],
    "destination": [{"id":"scripted","dst":[{}]}]
  })");
  expect(scripted.model &&
             hasDiagnostic(scripted, "skin_json_callback_unsupported") &&
             std::ranges::all_of(scripted.model->stringProperties,
                                 isStaticBinding<SkinStringPropertyBinding>),
         "a JSON Lua-script binding is diagnosed without creating a callback");
}

void testNegativeGenericGraphsKeepTheSelectOnlyGameplayBoundary() {
  const auto decoded = decodeInline(R"json({
    "type": 0,
    "source": [{"id":"atlas","path":"atlas.png"}],
    "graph": [
      {"id":"lamp-distribution","src":"atlas","w":11,"h":1,"divx":11,"type":-1},
      {"id":"rank-distribution","src":"atlas","w":28,"h":1,"divx":28,"type":-2}
    ],
    "destination": [
      {"id":"lamp-distribution","dst":[{}]},
      {"id":"rank-distribution","dst":[{}]}
    ]
  })json");
  const auto *lamp = decoded.model
                         ? findObject(*decoded.model, "lamp-distribution")
                         : nullptr;
  const auto *rank = decoded.model
                         ? findObject(*decoded.model, "rank-distribution")
                         : nullptr;
  expect(decoded.model && lamp && rank &&
             std::holds_alternative<SkinInvalidInGameplayObject>(
                 lamp->payload) &&
             std::holds_alternative<SkinInvalidInGameplayObject>(
                 rank->payload) &&
             !std::holds_alternative<SkinGraphObject>(lamp->payload) &&
             !std::holds_alternative<SkinGraphObject>(rank->payload) &&
             !std::holds_alternative<SkinNoteDistributionGraphObject>(
                 lamp->payload) &&
             !std::holds_alternative<SkinNoteDistributionGraphObject>(
                 rank->payload) &&
             hasDiagnostic(decoded,
                           "skin_json_distribution_graph_invalid_in_gameplay"),
         "negative generic Graph stays a diagnosed select-only object rather "
         "than becoming a gameplay graph");
}

void testTextRefWriterFallbackAndExplicitEventPrecedence() {
  const auto decoded = decodeInline(R"json({
    "type": 0,
    "font": [{"id":"font","path":"font.ttf"}],
    "text": [
      {"id":"omitted","font":"font","ref":30},
      {"id":"null","font":"font","ref":30,"event":null},
      {"id":"explicit","font":"font","ref":10,"event":30},
      {"id":"script","font":"font","ref":30,"event":"return value"}
    ],
    "destination": [
      {"id":"omitted","dst":[{}]},
      {"id":"null","dst":[{}]},
      {"id":"explicit","dst":[{}]},
      {"id":"script","dst":[{}]}
    ]
  })json");
  const auto *omitted = decoded.model
                            ? payload<SkinTextObject>(*decoded.model, "omitted")
                            : nullptr;
  const auto *nullEvent = decoded.model
                              ? payload<SkinTextObject>(*decoded.model, "null")
                              : nullptr;
  const auto *explicitEvent =
      decoded.model ? payload<SkinTextObject>(*decoded.model, "explicit")
                    : nullptr;
  const auto *script = decoded.model
                           ? payload<SkinTextObject>(*decoded.model, "script")
                           : nullptr;
  const auto writerSelector = [&](const SkinTextObject *text) {
    if (!decoded.model || text == nullptr || !text->writer ||
        text->writer->value == 0 ||
        text->writer->value > decoded.model->stringWriters.size()) {
      return std::optional<int>{};
    }
    const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
        &decoded.model->stringWriters[text->writer->value - 1].source);
    if (builtin == nullptr) return std::optional<int>{};
    const auto *numeric = std::get_if<int>(&builtin->value);
    return numeric == nullptr ? std::optional<int>{}
                              : std::optional<int>(*numeric);
  };
  expect(omitted && nullEvent && explicitEvent && script &&
             writerSelector(omitted) == 30 && omitted->editable &&
             writerSelector(nullEvent) == 30 && nullEvent->editable &&
             writerSelector(explicitEvent) == 30 && !explicitEvent->editable &&
             !script->writer && !script->editable &&
             hasDiagnostic(decoded, "skin_json_callback_unsupported"),
         "Text uses ref writer only for absent/null event and marks only the "
         "implicit writer editable");
}

void testJsonObjectDestinationAndMalformedFieldProvenance() {
  const auto decoded = decodeFixture("all_gameplay_fields.json");
  const auto *image = decoded.model
                          ? findObject(*decoded.model, "image-all")
                          : nullptr;
  const auto *destination =
      decoded.model && image ? findDestination(*decoded.model, image->id)
                             : nullptr;
  const auto nestedImage =
      decoded.model
          ? std::ranges::find_if(decoded.model->objects, [](const auto &object) {
              return object.authoredName == "__judge/judge-all/image/0";
            })
          : std::vector<SkinObjectDefinition>::const_iterator{};
  const auto nestedNumber =
      decoded.model
          ? std::ranges::find_if(decoded.model->objects, [](const auto &object) {
              return object.authoredName == "__judge/judge-all/number/0";
            })
          : std::vector<SkinObjectDefinition>::const_iterator{};
  const auto *judge = decoded.model
                          ? payload<SkinJudgeObject>(*decoded.model, "judge-all")
                          : nullptr;
  expect(image && destination && image->source.line == 67 &&
             image->source.column == 5 && destination->source.line == 445 &&
             destination->source.column == 5 &&
             image->source.line != destination->source.line,
         "object definitions and authored destinations retain independent "
         "exact JSON locations");
  expect(decoded.model && nestedImage != decoded.model->objects.end() &&
             nestedNumber != decoded.model->objects.end() &&
             nestedImage->source.line == 92 && nestedImage->source.column == 5 &&
             nestedNumber->source.line == 129 &&
             nestedNumber->source.column == 5 && judge &&
             judge->grades.front().image &&
             judge->grades.front().image->source.line == 406 &&
             judge->grades.front().image->source.column == 9 &&
             judge->grades.front().detailNumber &&
             judge->grades.front().detailNumber->source.line == 409 &&
             judge->grades.front().detailNumber->source.column == 9,
         "nested judge children retain independent definition and destination "
         "provenance");

  const auto malformed = decodeInline(R"json({
  "type": 0,
  "judge": [
    {
      "id": "judge",
      "index": "bad"
    }
  ],
  "destination": [{"id":"judge","dst":[]}]
})json");
  const auto found = std::ranges::find_if(
      malformed.diagnostics, [](const SkinDiagnostic &diagnostic) {
        return diagnostic.code == "skin_json_model_invalid" &&
               diagnostic.message.find("JsonSkin.Judge.index") !=
                   std::string::npos;
      });
  expect(found != malformed.diagnostics.end() && found->source &&
             found->source->virtualPath == "skin/inline.json" &&
             found->source->line == 6 && found->source->column == 16,
         "a malformed nested field diagnostic points at its exact JSON value");
}

void testCancellationStopsMidJsonModelFold() {
  Json document{{"type", 0}, {"source", Json::array()},
                {"image", Json::array()}, {"destination", Json::array()}};
  document["source"].push_back({{"id", "atlas"}, {"path", "atlas.png"}});
  for (int index = 0; index < 100; ++index) {
    const std::string id = "image-" + std::to_string(index);
    document["image"].push_back(
        {{"id", id}, {"src", "atlas"}, {"w", 1}, {"h", 1}});
    document["destination"].push_back({{"id", id}, {"dst", Json::array({Json::object()})}});
  }
  const std::string encoded = document.dump();
  std::stop_source source;
  CancellationCheckpoint checkpoint{.source = &source,
                                    .phase = StaticSkinDecodePhase::JsonModel,
                                    .stopAt = 12};
  const auto decoded = JsonGameplaySkinDecoder{}.decode(
      std::as_bytes(std::span(encoded)), fixtureEntry("cancel.json"), nullptr,
      gameplaySkinBuiltinCatalog(), SkinSafetyPolicy{}, source.get_token(),
      {.notify = requestCancellationAtCheckpoint, .context = &checkpoint});
  expect(decoded.cancelled && !decoded.model && decoded.diagnostics.empty(),
         "JSON cancellation is observed deterministically during model folding");
}

} // namespace

int main() {
  testAllFieldFixtureCoversThePinnedJsonLedger();
  testPinnedDefaultsProduceTypedStaticModel();
  testAllGameplayFieldsPreserveOrderProvenanceAndPayloads();
  testMalformedUnboundedAndCallbackJsonFailAtDecoderBoundary();
  testNegativeGenericGraphsKeepTheSelectOnlyGameplayBoundary();
  testTextRefWriterFallbackAndExplicitEventPrecedence();
  testJsonObjectDestinationAndMalformedFieldProvenance();
  testCancellationStopsMidJsonModelFold();

  if (failures != 0) {
    std::cerr << failures << " JSON gameplay skin decoder test(s) failed\n";
    return 1;
  }
  std::cout << "JSON gameplay skin decoder tests passed\n";
  return 0;
}
