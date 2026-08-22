#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/Lr2GameplaySkinDecoder.h"
#include "gameplay_skin_ledger_evidence.h"
#include "skin/beatoraja/Lr2IntegerParser.h"
#include "skin/beatoraja/Lr2SkinHeaderDecoder.h"
#include "skin/beatoraja/SkinModelValidator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
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

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](char character) {
    return character >= 'a' && character <= 'z'
               ? static_cast<char>(character - ('a' - 'A'))
               : character;
  });
  return value;
}

std::vector<std::string> splitFields(std::string_view line) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      result.emplace_back(line.substr(start));
      return result;
    }
    result.emplace_back(line.substr(start, comma - start));
    start = comma + 1;
  }
}

std::vector<Lr2SkinCommand> readFixture() {
  const fs::path path = fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/lr2/"
                        "all_play_commands.lr2skin";
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "LR2 all-command fixture opens");
  std::vector<Lr2SkinCommand> commands;
  std::string line;
  std::uint32_t lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() != '#') continue;
    auto tokens = splitFields(line);
    std::string name = upperAscii(tokens.front().substr(1));
    tokens.erase(tokens.begin());
    commands.push_back(
        {.name = std::move(name),
         .fields = std::move(tokens),
         .source = {.virtualPath = "all_play_commands.lr2skin",
                    .line = lineNumber,
                    .column = 1},
         .includeChain = {"all_play_commands.lr2skin"}});
  }
  return commands;
}

constexpr auto kRegisteredCommands = std::to_array<std::string_view>({
    "INCLUDE", "IMAGE", "LR2FONT", "SRC_IMAGE", "IMAGESET",
    "SRC_IMAGESET", "DST_IMAGE", "SRC_NUMBER", "DST_NUMBER", "SRC_TEXT",
    "DST_TEXT", "SRC_SLIDER", "SRC_SLIDER_REFNUMBER", "DST_SLIDER",
    "SRC_BARGRAPH", "SRC_BARGRAPH_REFNUMBER", "DST_BARGRAPH", "SRC_BUTTON",
    "DST_BUTTON", "SRC_ONMOUSE", "DST_ONMOUSE", "SRC_GROOVEGAUGE",
    "SRC_GROOVEGAUGE_EX", "DST_GROOVEGAUGE", "STARTINPUT", "SCENETIME",
    "FADEOUT", "STRETCH", "FINISHMARGIN", "JUDGETIMER", "SRC_BGA",
    "DST_BGA", "SRC_LINE", "DST_LINE", "SRC_NOTE", "SRC_LN_END",
    "SRC_LN_START", "SRC_LN_BODY", "SRC_LN_BODY_INACTIVE",
    "SRC_LN_BODY_ACTIVE", "SRC_HCN_END", "SRC_HCN_START", "SRC_HCN_BODY",
    "SRC_HCN_BODY_INACTIVE", "SRC_HCN_BODY_ACTIVE", "SRC_HCN_DAMAGE",
    "SRC_HCN_REACTIVE", "SRC_MINE", "DST_NOTE", "DST_NOTE2",
    "DST_NOTE_EXPANSION_RATE", "SRC_NOWJUDGE_1P", "DST_NOWJUDGE_1P",
    "SRC_NOWJUDGE_2P", "DST_NOWJUDGE_2P", "SRC_NOWJUDGE_3P",
    "DST_NOWJUDGE_3P", "SRC_NOWCOMBO_1P", "DST_NOWCOMBO_1P",
    "SRC_NOWCOMBO_2P", "DST_NOWCOMBO_2P", "SRC_NOWCOMBO_3P",
    "DST_NOWCOMBO_3P", "SRC_JUDGELINE", "DST_JUDGELINE",
    "SRC_NOTECHART_1P", "DST_NOTECHART_1P", "SRC_BPMCHART",
    "DST_BPMCHART", "SRC_TIMING_1P", "DST_TIMING_1P", "SRC_HIDDEN",
    "DST_HIDDEN", "SRC_LIFT", "DST_LIFT", "DST_PM_CHARA_1P",
    "DST_PM_CHARA_2P", "DST_PM_CHARA_ANIMATION", "SRC_PM_CHARA_IMAGE",
    "DST_PM_CHARA_IMAGE", "CLOSE", "PLAYSTART", "LOADSTART", "LOADEND",
});

void testFixtureCoversEveryPinnedRegistration(
    std::span<const Lr2SkinCommand> commands) {
  expect(kRegisteredCommands.size() == 84,
         "the pinned CSV/play loader inventory has exactly 84 registrations");
  std::set<std::string, std::less<>> observed;
  for (const auto &command : commands) observed.insert(command.name);
  for (const auto name : kRegisteredCommands) {
    expect(observed.contains(name),
           std::string("all-command fixture covers #") + std::string(name));
  }
}

const Lr2SkinCommand *commandNamed(std::span<const Lr2SkinCommand> commands,
                                   std::string_view name,
                                   std::size_t occurrence = 0) {
  for (const auto &command : commands) {
    if (command.name == name && occurrence-- == 0) return &command;
  }
  return nullptr;
}

const SkinObjectDefinition *objectFromCommand(
    const BeatorajaSkinModel &model, std::span<const Lr2SkinCommand> commands,
    std::string_view name, std::size_t occurrence = 0) {
  const auto *command = commandNamed(commands, name, occurrence);
  if (command == nullptr) return nullptr;
  const auto found = std::ranges::find_if(
      model.objects, [&](const auto &object) {
        return object.source.virtualPath == command->source.virtualPath &&
               object.source.line == command->source.line;
      });
  return found == model.objects.end() ? nullptr : &*found;
}

const SkinDestination *destinationFor(const BeatorajaSkinModel &model,
                                      SkinObjectId object) {
  const auto found = std::ranges::find_if(
      model.destinations,
      [&](const auto &destination) { return destination.object == object; });
  return found == model.destinations.end() ? nullptr : &*found;
}

template <typename Payload>
std::vector<const SkinObjectDefinition *>
objectsWith(const BeatorajaSkinModel &model) {
  std::vector<const SkinObjectDefinition *> result;
  for (const auto &object : model.objects) {
    if (std::holds_alternative<Payload>(object.payload)) {
      result.push_back(&object);
    }
  }
  return result;
}

template <typename Binding, typename Id>
std::optional<int> numericSelector(std::span<const Binding> bindings, Id id) {
  const auto found = std::ranges::find_if(
      bindings, [&](const auto &binding) { return binding.id == id; });
  if (found == bindings.end()) return std::nullopt;
  const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(&found->source);
  if (builtin == nullptr) return std::nullopt;
  const auto *numeric = std::get_if<int>(&builtin->value);
  return numeric == nullptr ? std::nullopt : std::optional<int>(*numeric);
}

bool hasCode(const Lr2GameplaySkinDecodeResult &decoded,
             std::string_view fragment) {
  return std::ranges::any_of(decoded.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.code.find(fragment) != std::string::npos;
  });
}

struct CancellationCheckpoint {
  std::stop_source *source = nullptr;
  std::size_t stopAt = 0;
};

void requestCancellationAtCheckpoint(StaticSkinDecodePhase phase,
                                     std::size_t workItem,
                                     void *context) noexcept {
  auto &checkpoint = *static_cast<CancellationCheckpoint *>(context);
  if (phase == StaticSkinDecodePhase::Lr2Model &&
      workItem == checkpoint.stopAt) {
    checkpoint.source->request_stop();
  }
}

bool rectEquals(const SkinAuthoredRect &left, const SkinAuthoredRect &right) {
  return left.x == right.x && left.y == right.y &&
         left.width == right.width && left.height == right.height;
}

void testAllCommandsDecodeToTypedCanonicalModel() {
  const auto commands = readFixture();
  testFixtureCoversEveryPinnedRegistration(commands);

  const auto headerResult = Lr2SkinHeaderDecoder{}.decode(commands);
  expect(headerResult.header.has_value() && headerResult.diagnostics.empty(),
         "all-command LR2 header decodes before gameplay folding");
  if (!headerResult.header) return;

  EntryProfileSettings desired;
  desired.options["Theme"] = 901;
  desired.filePaths["Files"] = "selected.png";
  desired.offsets["Move"] = {.x = 7, .y = 8, .w = 9, .h = 10, .r = 11,
                              .a = 12};
  desired.viewport = {.mode = ViewportMode::Stretch,
                      .scaleX = 1.25F,
                      .scaleY = 0.75F,
                      .translateX = 12.0F,
                      .translateY = -8.0F};

  const auto decoded = Lr2GameplaySkinDecoder{}.decode(
      *headerResult.header, commands, &desired, gameplaySkinBuiltinCatalog());
  expect(decoded.configuration.has_value() &&
             decoded.reconciledSettings.has_value() && decoded.model.has_value(),
         "all registered LR2 commands produce a complete static document");
  expect(!hasCode(decoded, "unsupported"),
         "registered gameplay commands and unknown non-gameplay commands do not become unsupported placeholders");
  if (!decoded.configuration || !decoded.reconciledSettings || !decoded.model) {
    return;
  }
  const auto &configuration = *decoded.configuration;
  const auto &model = *decoded.model;

  expect(configuration.options.at("Theme") == 901 &&
             configuration.enabledOptionIds.contains(901) &&
             configuration.optionStates.at(900) == 0 &&
             configuration.optionStates.at(901) == 1 &&
             configuration.filePaths.at("Files") == "selected.png" &&
             configuration.offsetsById.at(77) ==
                 ConfigOffset{.x = 7, .y = 8, .w = 9, .h = 10, .r = 11,
                              .a = 12} &&
             decoded.reconciledSettings->viewport == desired.viewport,
         "LR2 configured options, files, offsets, and viewport reconcile by authored name");
  expect(configuration.lowercaseSha256 ==
             skinConfigurationDigest(*decoded.reconciledSettings),
         "LR2 reconciled configuration publishes the exact durable digest");

  expect(model.header.type == 0 && model.header.width == 640 &&
             model.header.height == 480 && model.timing.inputMillis == 101 &&
             model.timing.sceneMillis == 202 &&
             model.timing.fadeoutMillis == 303 &&
             model.timing.finishMarginMillis == 404 &&
             model.timing.judgeTimerMillis == 4 &&
             model.timing.closeMillis == 505 &&
             model.timing.playStartMillis == 606 &&
             model.timing.loadStartMillis == 707 &&
             model.timing.loadEndMillis == 808,
         "conditionals, SETOPTION, and every common/play timing command fold exactly");

  expect(std::ranges::none_of(model.objects, [](const auto &object) {
           return std::holds_alternative<SkinBlankObject>(object.payload);
         }),
         "no registered LR2 gameplay command materializes a blank object");
  expect(std::ranges::all_of(model.objects, [](const auto &object) {
           return object.source.line > 0 &&
                  object.source.virtualPath == "all_play_commands.lr2skin";
         }),
         "every LR2 object retains exact command provenance");

  const auto *imageDefinition = objectFromCommand(model, commands, "SRC_IMAGE");
  const auto *image = imageDefinition == nullptr
                          ? nullptr
                          : std::get_if<SkinImageObject>(&imageDefinition->payload);
  const auto *imageDestination =
      imageDefinition == nullptr ? nullptr
                                 : destinationFor(model, imageDefinition->id);
  expect(image != nullptr && image->orderedStates.size() == 1 &&
             image->orderedStates[0].frames.size() == 1 &&
             image->orderedStates[0].frames[0].w == -1 &&
             image->orderedStates[0].frames[0].h == -1 &&
             image->orderedStates[0].frames[0].gridColumns == 1 &&
             image->orderedStates[0].frames[0].gridRows == 1 &&
             image->orderedStates[0].cycleMillis == 0 &&
             !image->orderedStates[0].timer,
         "SRC_IMAGE applies pinned full-texture, one-division, zero-cycle defaults");
  expect(imageDestination != nullptr &&
             imageDestination->presentation.frames.size() == 2 &&
             imageDestination->presentation.frames[0].timeMillis == 100 &&
             imageDestination->presentation.frames[1].timeMillis == 200 &&
             imageDestination->presentation.frames[0].x == -20 &&
             imageDestination->presentation.frames[0].y == 460 &&
             imageDestination->presentation.frames[0].width == 30 &&
             imageDestination->presentation.frames[0].height == 40 &&
             imageDestination->presentation.frames[0].rgba ==
                 std::array<std::uint8_t, 4>{0, 202, 203, 255} &&
             imageDestination->presentation.blend ==
                 SkinBlendMode::Additive &&
             imageDestination->presentation.filter == SkinFilterMode::Linear &&
             imageDestination->presentation.stretch ==
                 SkinStretchMode::KeepAspectRatioFitOuter &&
             imageDestination->presentation.loop == -1 &&
             imageDestination->source.line ==
                 commandNamed(commands, "DST_IMAGE", 0)->source.line,
         "DST_IMAGE clamps Color channels, normalizes dimensions, converts 480-top Y, stably sorts frames, and keeps provenance");
  expect(imageDestination != nullptr && imageDestination->presentation.timer &&
             numericSelector(std::span(model.timerProperties),
                             *imageDestination->presentation.timer) == 70 &&
             imageDestination->presentation.offsetIds ==
                 std::vector<int>{77},
         "destination timer and first offset association retain numeric selectors");
  const auto *movieDefinition =
      objectFromCommand(model, commands, "SRC_IMAGE", 1);
  const auto *movie = movieDefinition == nullptr
                          ? nullptr
                          : std::get_if<SkinImageObject>(
                                &movieDefinition->payload);
  expect(movie != nullptr && movie->orderedStates.size() == 1 &&
             movie->orderedStates[0].frames.size() == 1 &&
             movie->orderedStates[0].frames[0].x == 0 &&
             movie->orderedStates[0].frames[0].y == 0 &&
             movie->orderedStates[0].frames[0].w == -1 &&
             movie->orderedStates[0].frames[0].h == -1 &&
             movie->orderedStates[0].cycleMillis == 0 &&
             !movie->orderedStates[0].timer,
         "movie-backed SRC_IMAGE ignores sprite crop/division/timer fields like SkinSourceMovie");
  expect(std::ranges::any_of(model.resources, [](const auto &definition) {
           const auto *resource = std::get_if<SkinMovieResource>(&definition);
           return resource != nullptr &&
                  resource->virtualPath == "skin/movie.mp4";
         }),
         "movie-backed IMAGE is retained as an explicit movie resource");
  bool configuredCondition = false;
  bool booleanCondition = false;
  if (imageDestination != nullptr) {
    for (const auto &condition : imageDestination->presentation.conditions) {
      if (const auto *option = std::get_if<int>(&condition)) {
        configuredCondition = configuredCondition || *option == 901;
      } else if (const auto *property =
                     std::get_if<SkinBooleanPropertyId>(&condition)) {
        booleanCondition =
            booleanCondition ||
            numericSelector(std::span(model.booleanProperties), *property) ==
                -100;
      }
    }
  }
  expect(configuredCondition && booleanCondition,
         "DST options distinguish configured IDs from built-in boolean selectors");

  const auto *imageSetDefinition =
      objectFromCommand(model, commands, "SRC_IMAGESET");
  const auto *imageSet = imageSetDefinition == nullptr
                             ? nullptr
                             : std::get_if<SkinImageObject>(
                                   &imageSetDefinition->payload);
  expect(imageSet != nullptr && imageSet->orderedStates.size() == 1 &&
             imageSet->stateIndex &&
             numericSelector(std::span(model.integerProperties),
                             *imageSet->stateIndex) == 301,
         "IMAGESET/SRC_IMAGESET retain ordered states and image-index selector IDs");

  const auto *numberDefinition =
      objectFromCommand(model, commands, "SRC_NUMBER");
  const auto *number = numberDefinition == nullptr
                           ? nullptr
                           : std::get_if<SkinNumberObject>(
                                 &numberDefinition->payload);
  expect(number != nullptr && number->digitCount == 4 && number->spacing == 3 &&
             number->alignment == 2 &&
             number->digits.glyphsPerAnimationFrame == 11 &&
             number->digits.positive.frames.size() == 11 &&
             number->zeroPadding == SkinZeroPaddingMode::AlternateZero &&
             numericSelector(std::span(model.integerProperties),
                             number->value) == 10,
         "SRC_NUMBER truncates a partial 11-glyph row like the pinned atlas constructor");

  const auto *textDefinition = objectFromCommand(model, commands, "SRC_TEXT");
  const auto *text = textDefinition == nullptr
                         ? nullptr
                         : std::get_if<SkinTextObject>(&textDefinition->payload);
  expect(text != nullptr && text->font != 0 && text->alignment == 1 &&
             text->editable && text->value &&
             numericSelector(std::span(model.stringProperties), *text->value) ==
                 10,
         "LR2FONT/SRC_TEXT produce a bitmap-font text object with the pinned selector");
  expect(std::ranges::any_of(model.resources, [](const auto &definition) {
           const auto *font = std::get_if<SkinFontResource>(&definition);
           return font != nullptr && font->virtualPath == "skin/font.lr2font" &&
                  font->bitmap &&
                  font->bitmap->virtualPath == "skin/font.lr2font";
         }),
         "LR2FONT uses the shared typed LR2 bitmap-font resource policy");

  const auto sliders = objectsWith<SkinSliderObject>(model);
  expect(sliders.size() == 2,
         "ordinary and refnumber slider commands remain separate objects");
  if (sliders.size() == 2) {
    const auto &ordinary = std::get<SkinSliderObject>(sliders[0]->payload);
    const auto &reference = std::get<SkinSliderObject>(sliders[1]->payload);
    const auto *rate = std::get_if<SkinFloatPropertyId>(&ordinary.value);
    const auto *integer =
        std::get_if<SkinSliderObject::IntegerRangeSource>(&reference.value);
    expect(rate != nullptr &&
               numericSelector(std::span(model.floatProperties), *rate) == 4 &&
               ordinary.direction == 1 && ordinary.range == 90 &&
               ordinary.changeable && integer != nullptr &&
               numericSelector(std::span(model.integerProperties),
                               integer->value) == 10 &&
               integer->minimum == 0 && integer->maximum == 100,
           "slider overloads retain rate versus bounded integer selector domains");
  }

  const auto graphs = objectsWith<SkinGraphObject>(model);
  expect(graphs.size() == 2,
         "ordinary and refnumber bar graphs remain typed graph objects");
  if (graphs.size() == 2) {
    const auto &ordinary = std::get<SkinGraphObject>(graphs[0]->payload);
    const auto &reference = std::get<SkinGraphObject>(graphs[1]->payload);
    const auto *rate = std::get_if<SkinFloatPropertyId>(&ordinary.value);
    const auto *integer =
        std::get_if<SkinSliderObject::IntegerRangeSource>(&reference.value);
    expect(rate != nullptr &&
               numericSelector(std::span(model.floatProperties), *rate) == 101 &&
               ordinary.direction == 0 && integer != nullptr &&
               numericSelector(std::span(model.integerProperties),
                               integer->value) == 10 &&
               reference.direction == 1,
           "bar graph selector +100 and refnumber ranges follow pinned constructors");
  }

  const auto *buttonDefinition = objectFromCommand(model, commands, "SRC_BUTTON");
  const auto *button = buttonDefinition == nullptr
                           ? nullptr
                           : std::get_if<SkinImageObject>(
                                 &buttonDefinition->payload);
  expect(button != nullptr && button->orderedStates.size() == 2 &&
             button->stateIndex && button->clickEvent &&
             numericSelector(std::span(model.integerProperties),
                             *button->stateIndex) == 301 &&
             numericSelector(std::span(model.events), *button->clickEvent) ==
                 301 &&
             button->clickMode == 1,
         "SRC_BUTTON retains state, click selector, grouping, and decrement mode");

  const auto *mouseDefinition =
      objectFromCommand(model, commands, "SRC_ONMOUSE");
  const auto *mouseDestination =
      mouseDefinition == nullptr ? nullptr
                                 : destinationFor(model, mouseDefinition->id);
  expect(mouseDestination != nullptr &&
             mouseDestination->presentation.mouseRect &&
             rectEquals(*mouseDestination->presentation.mouseRect,
                        SkinAuthoredRect{.x = 2,
                                         .y = 7,
                                         .width = 30,
                                         .height = 10}),
         "SRC_ONMOUSE preserves its source-relative hit rectangle");

  const auto gauges = objectsWith<SkinGaugeObject>(model);
  expect(gauges.size() == 2,
         "legacy and EX groove gauge commands create distinct typed gauges");
  if (gauges.size() == 2) {
    const auto &legacy = std::get<SkinGaugeObject>(gauges[0]->payload);
    const auto &extended = std::get<SkinGaugeObject>(gauges[1]->payload);
    expect(legacy.orderedNodes.size() == 36 && legacy.parts == 50 &&
               legacy.animation == SkinGaugeAnimationType::Random &&
               legacy.animationRange == 3 &&
               legacy.animationCycleMillis == 33 &&
               legacy.resultEndMillis == 500 &&
               extended.orderedNodes.size() == 36 && extended.parts == 8 &&
               extended.animation == SkinGaugeAnimationType::Increase &&
               extended.resultStartMillis == 10 &&
               extended.resultEndMillis == 510,
           "groove gauges transpose LR2 node sheets into all 36 canonical roles");
  }

  const auto bgaObjects = objectsWith<SkinBgaObject>(model);
  const auto *bgaDestination =
      bgaObjects.empty() ? nullptr : destinationFor(model, bgaObjects[0]->id);
  expect(bgaObjects.size() == 1 && bgaDestination != nullptr &&
             bgaDestination->presentation.frames.size() == 1 &&
             bgaDestination->presentation.frames[0].timeMillis == 0,
         "SRC/DST_BGA create the typed BGA object and force the pinned zero destination time");
  const auto notes = objectsWith<SkinNoteObject>(model);
  expect(notes.size() == 1,
         "all NOTE/LN/HCN/MINE commands converge into one typed note object");
  if (notes.size() == 1) {
    const auto &note = std::get<SkinNoteObject>(notes.front()->payload);
    expect(note.lanes.size() == 8 && note.lines.size() == 4 &&
               note.expansionRatePercent == std::array<int, 2>{125, 150} &&
               note.lanes[0].authoredLane == 0 &&
               rectEquals(note.lanes[0].laneDestination,
                          SkinAuthoredRect{.x = 20,
                                           .y = 80,
                                           .width = 50,
                                           .height = 350}) &&
               note.lanes[0].authoredNoteHeight == 300 &&
               note.lanes[0].secondaryDestinationY == -20 &&
               note.lanes[0].visuals.contains(SkinNoteVisualKind::Normal) &&
               note.lanes[0].visuals.contains(SkinNoteVisualKind::LnBodyActive) &&
               note.lanes[0].visuals.contains(SkinNoteVisualKind::HcnDamage),
           "lane mapping, lane-cover height adjustment, 480-Y geometry, note height, NOTE2, expansion, lines, and every long-note role materialize");
    expect(note.lines.size() == 4 && note.lines[1].sprite &&
               note.lines[1].destination && note.lines[2].sprite &&
               note.lines[2].destination && note.lines[3].sprite &&
               note.lines[3].destination &&
               note.lines[1].destination->frames.front().height == 8 &&
               note.lines[1].destination->frames.front().rgba ==
                   std::array<std::uint8_t, 4>{0, 192, 0, 255} &&
               note.lines[2].destination->frames.front().rgba ==
                   std::array<std::uint8_t, 4>{192, 192, 0, 255} &&
               note.lines[3].destination->frames.front().rgba ==
                   std::array<std::uint8_t, 4>{64, 192, 192, 255},
           "missing BPM, stop, and time lines use pinned system-pixel defaults");
    const auto *noteDestination = destinationFor(model, notes.front()->id);
    expect(noteDestination != nullptr &&
               noteDestination->presentation.offsetIds ==
                   std::vector<int>{30, 77},
           "DST_NOTE prepends the pinned notes offset before authored offsets");
  }

  const auto judges = objectsWith<SkinJudgeObject>(model);
  expect(judges.size() == 3,
         "NOWJUDGE/NOWCOMBO commands create all three player judge objects");
  std::set<int> judgePlayers;
  for (const auto *definition : judges) {
    const auto &judge = std::get<SkinJudgeObject>(definition->payload);
    judgePlayers.insert(judge.player);
    expect(judge.grades.size() == 7 && judge.grades[0].image &&
               judge.grades[0].detailNumber,
           "judge grade zero associates its latest image and relative combo number");
    if (judge.grades.size() == 7 && judge.grades[0].detailNumber) {
      const auto child = std::ranges::find_if(
          model.objects, [&](const auto &candidate) {
            return candidate.id == judge.grades[0].detailNumber->object;
          });
      const auto *combo = child == model.objects.end()
                              ? nullptr
                              : std::get_if<SkinNumberObject>(&child->payload);
      expect(combo != nullptr && combo->relativeToJudgeImage,
             "NOWCOMBO numbers retain judge-relative placement");
    }
  }
  expect(judgePlayers == std::set<int>{0, 1, 2},
         "judge player indices are zero-based and stable");

  const auto noteCharts = objectsWith<SkinNoteDistributionGraphObject>(model);
  const auto bpmCharts = objectsWith<SkinBpmGraphObject>(model);
  const auto timingCharts = objectsWith<SkinTimingVisualizerObject>(model);
  expect(noteCharts.size() == 1 && bpmCharts.size() == 1 &&
             timingCharts.size() == 1,
         "NOTECHART, BPMCHART, and TIMING remain distinct typed visualizers");
  if (!noteCharts.empty()) {
    const auto &chart =
        std::get<SkinNoteDistributionGraphObject>(noteCharts.front()->payload);
    expect(chart.type == SkinNoteDistributionGraphType::Judge &&
               chart.delayMillis == 500 && chart.backgroundTextureOff &&
               chart.reverseOrder && chart.noGap && chart.noHorizontalGap,
           "NOTECHART flags use the pinned LR2 constructor fields");
    const auto *destination =
        destinationFor(model, noteCharts.front()->id);
    expect(destination != nullptr &&
               !destination->presentation.frames.empty() &&
               destination->presentation.frames.front().width == 222 &&
               destination->presentation.frames.front().height == 77,
           "graph destinations use the pinned loader's single shared source rectangle");
  }
  if (!bpmCharts.empty()) {
    const auto &chart = std::get<SkinBpmGraphObject>(bpmCharts.front()->payload);
    expect(chart.delayMillis == 250 && chart.lineWidth == 2 &&
               chart.mainRgba == 0x00ff00ffU &&
               chart.transitionRgba == 0x7f7f7fffU,
           "BPMCHART timing, line width, and six colors decode exactly");
  }
  if (!timingCharts.empty()) {
    const auto &chart =
        std::get<SkinTimingVisualizerObject>(timingCharts.front()->payload);
    expect(chart.width == 301 && chart.judgeWidthMillis == 150 &&
               chart.lineWidth == 2 && chart.transparent && chart.drawDecay &&
               chart.lineRgba == 0x00ff00ffU &&
               chart.centerRgba == 0xffffffffU,
           "TIMING clamps line width and preserves color/decay flags");
  }

  const auto covers = objectsWith<SkinCoverObject>(model);
  expect(covers.size() == 2,
         "HIDDEN and LIFT source/destination pairs remain separate covers");
  if (covers.size() == 2) {
    const auto &hidden = std::get<SkinCoverObject>(covers[0]->payload);
    const auto &lift = std::get<SkinCoverObject>(covers[1]->payload);
    expect(hidden.kind == SkinCoverKind::Hidden &&
               hidden.disappearLine == 180 &&
               !hidden.disappearLineLinksLift &&
               lift.kind == SkinCoverKind::Lift && lift.disappearLine == -1 &&
               lift.disappearLineLinksLift,
           "cover disappearance-line defaults and 480 conversion match the pinned loader");
    const auto *hiddenDestination = destinationFor(model, covers[0]->id);
    const auto *liftDestination = destinationFor(model, covers[1]->id);
    expect(hiddenDestination != nullptr && liftDestination != nullptr &&
               hiddenDestination->presentation.offsetIds ==
                   std::vector<int>{3, 5, 77} &&
               liftDestination->presentation.offsetIds ==
                   std::vector<int>{3, 77},
           "HIDDEN/LIFT prepend their exact built-in offset families");
  }

  const auto pmCharacters = objectsWith<SkinPmCharaObject>(model);
  expect(pmCharacters.size() == 4,
         "all four LR2 PM-character command paths create typed objects");
  if (pmCharacters.size() == 4) {
    std::multiset<std::tuple<int, int, int>> variants;
    for (const auto *definition : pmCharacters) {
      const auto &pm = std::get<SkinPmCharaObject>(definition->payload);
      variants.emplace(pm.type, pm.color, pm.side);
      expect(pm.sourcePath == "skin/chara",
             "PM-character path remains available to the resource planner");
    }
    expect(variants.contains({0, 2, 1}) && variants.contains({0, 1, 2}) &&
               variants.contains({8, 1, 1}) && variants.contains({5, 2, 1}),
           "PM play, animation, static image, color, type, and side mapping is exact");
  }

  const auto validation = SkinModelValidator{}.validate(
      model,
      {.builtins = gameplaySkinBuiltinCatalog(), .callbacks = std::nullopt});
  expect(validation.model.has_value() && !validation.criticalFailure,
         "the complete LR2 document validates as a static canonical model");
}

void testBuiltinReferenceBarGraphsRetainTheirPinnedSource() {
  const BeatorajaSkinHeader header{.type = 0, .width = 640, .height = 480};
  const std::vector<Lr2SkinCommand> commands{
      {.name = "SRC_BARGRAPH",
       .fields = {"0", "110", "0", "0", "10", "10", "1", "1", "0",
                  "0", "1", "0"},
       .source = {.virtualPath = "builtin.lr2skin", .line = 1, .column = 1},
       .includeChain = {"builtin.lr2skin"}},
      {.name = "DST_BARGRAPH",
       .fields = {"0", "0", "0", "0", "100", "20", "0", "255", "255",
                  "255", "255", "0", "0", "0", "0", "0", "0", "0",
                  "0", "0"},
       .source = {.virtualPath = "builtin.lr2skin", .line = 2, .column = 1},
       .includeChain = {"builtin.lr2skin"}},
      {.name = "SRC_BARGRAPH_REFNUMBER",
       .fields = {"0", "111", "0", "0", "10", "10", "1", "1", "0",
                  "0", "14", "1", "0", "100"},
       .source = {.virtualPath = "builtin.lr2skin", .line = 3, .column = 1},
       .includeChain = {"builtin.lr2skin"}},
      {.name = "DST_BARGRAPH",
       .fields = {"0", "0", "0", "30", "100", "20", "0", "255", "255",
                  "255", "255", "0", "0", "0", "0", "0", "0", "0",
                  "0", "0"},
       .source = {.virtualPath = "builtin.lr2skin", .line = 4, .column = 1},
       .includeChain = {"builtin.lr2skin"}},
  };
  const auto decoded = Lr2GameplaySkinDecoder{}.decode(
      header, commands, nullptr, gameplaySkinBuiltinCatalog());
  const auto graphs = decoded.model ? objectsWith<SkinGraphObject>(*decoded.model)
                                    : std::vector<const SkinObjectDefinition *>{};
  const auto *ordinary = graphs.size() > 0
                             ? std::get_if<SkinGraphObject>(&graphs[0]->payload)
                             : nullptr;
  const auto *refNumber = graphs.size() > 1
                              ? std::get_if<SkinGraphObject>(&graphs[1]->payload)
                              : nullptr;
  expect(decoded.model && ordinary && refNumber &&
             ordinary->builtinImageReference == 110 &&
             refNumber->builtinImageReference == 111 &&
             ordinary->fill.frames.empty() && refNumber->fill.frames.empty(),
         "SRC_BARGRAPH and REFNUMBER retain gr>=100 as built-in image sources");
}

void testStrictLr2IntegerMatchesJavaParseIntAsciiBoundary() {
  expect(parseLr2JavaInteger("0") == 0 &&
             parseLr2JavaInteger("+2147483647") ==
                 std::numeric_limits<int>::max() &&
             parseLr2JavaInteger("-2147483648") ==
                 std::numeric_limits<int>::min() &&
             !parseLr2JavaInteger("") && !parseLr2JavaInteger("+") &&
             !parseLr2JavaInteger(" 1") &&
             !parseLr2JavaInteger("1 ") &&
             !parseLr2JavaInteger("id=777") &&
             !parseLr2JavaInteger("1junk") &&
             !parseLr2JavaInteger("2147483648") &&
             !parseLr2JavaInteger("-2147483649"),
         "strict LR2 integers accept Java ASCII signs and 32-bit endpoints "
         "without trimming, coercion, or overflow");
}

void testCancellationStopsMidLr2ModelFold() {
  const BeatorajaSkinHeader header{.type = 0, .width = 640, .height = 480};
  std::vector<Lr2SkinCommand> commands;
  for (std::uint32_t line = 1; line <= 100; ++line) {
    commands.push_back(
        {.name = "STARTINPUT",
         .fields = {std::to_string(line)},
         .source = {.virtualPath = "cancel.lr2skin", .line = line, .column = 1},
         .includeChain = {"cancel.lr2skin"}});
  }
  std::stop_source source;
  CancellationCheckpoint checkpoint{.source = &source, .stopAt = 7};
  const auto decoded = Lr2GameplaySkinDecoder{}.decode(
      header, commands, nullptr, gameplaySkinBuiltinCatalog(),
      SkinSafetyPolicy{}, source.get_token(),
      {.notify = requestCancellationAtCheckpoint, .context = &checkpoint});
  expect(decoded.cancelled && !decoded.model && decoded.diagnostics.empty(),
         "LR2 cancellation is observed deterministically during command folding");
}

} // namespace

int main(int argc, char **argv) {
  testStrictLr2IntegerMatchesJavaParseIntAsciiBoundary();
  testBuiltinReferenceBarGraphsRetainTheirPinnedSource();
  testCancellationStopsMidLr2ModelFold();
  testAllCommandsDecodeToTypedCanonicalModel();
  return gameplay_skin_ledger_evidence::finish(
      argc, argv, "lr2_gameplay_skin_decoder_tests", failures,
      "LR2 gameplay skin decoder test(s) failed",
      "LR2 gameplay skin decoder tests passed");
}
