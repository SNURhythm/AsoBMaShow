#include "GameplaySkinSettingsPresentation.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace skin {
namespace {

class PresentationKeyEncoder {
public:
  void boolean(bool value) {
    value_.push_back('B');
    value_.push_back(value ? '1' : '0');
  }

  void signedNumber(std::int64_t value) {
    value_.push_back('I');
    appendDecimal(value);
    value_.push_back(';');
  }

  void unsignedNumber(std::uint64_t value) {
    value_.push_back('U');
    appendDecimal(value);
    value_.push_back(';');
  }

  void floatingPoint(float value) {
    value_.push_back('F');
    appendDecimal(std::bit_cast<std::uint32_t>(value));
    value_.push_back(';');
  }

  void text(std::string_view value) {
    value_.push_back('S');
    appendDecimal(value.size());
    value_.push_back(':');
    value_.append(value);
  }

  [[nodiscard]] std::string finish() && { return std::move(value_); }

private:
  template <typename Number> void appendDecimal(Number value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    value_.append(buffer, result.ptr);
  }

  std::string value_;
};

template <typename Enum>
void encodeEnum(PresentationKeyEncoder &encoder, Enum value) {
  encoder.unsignedNumber(static_cast<std::uint64_t>(value));
}

void encodePackage(PresentationKeyEncoder &encoder,
                   const SkinPackageId &package) {
  encoder.text(package.directoryName);
  encoder.text(package.collisionKey);
}

void encodeEntry(PresentationKeyEncoder &encoder, const SkinEntryId &entry) {
  encodePackage(encoder, entry.package);
  encoder.text(entry.packageRelativePath);
  encoder.text(entry.collisionKey);
}

void encodeDiagnostic(PresentationKeyEncoder &encoder,
                      const SkinDiagnostic &diagnostic) {
  encoder.text(diagnostic.code);
  encoder.text(diagnostic.message);
  encoder.text(diagnostic.virtualPath);
  encodeEnum(encoder, diagnostic.severity);
  encoder.boolean(diagnostic.source.has_value());
  if (diagnostic.source) {
    encoder.text(diagnostic.source->virtualPath);
    encoder.unsignedNumber(diagnostic.source->line);
    encoder.unsignedNumber(diagnostic.source->column);
  }
}

void encodeMetadata(PresentationKeyEncoder &encoder,
                    const SkinEntryMetadataSnapshot &metadata) {
  encoder.text(metadata.displayName);
  encoder.text(metadata.author);
  encoder.signedNumber(metadata.skinType);
  encoder.signedNumber(metadata.authoredWidth);
  encoder.signedNumber(metadata.authoredHeight);

  encoder.unsignedNumber(metadata.categories.size());
  for (const auto &category : metadata.categories) {
    encoder.text(category.name);
    encoder.unsignedNumber(category.items.size());
    for (const auto &item : category.items) {
      encoder.text(item);
    }
  }

  encoder.unsignedNumber(metadata.options.size());
  for (const auto &option : metadata.options) {
    encoder.text(option.category);
    encoder.text(option.name);
    encoder.unsignedNumber(option.choices.size());
    for (const auto &choice : option.choices) {
      encoder.text(choice.label);
      encoder.signedNumber(choice.value);
    }
    encoder.text(option.defaultLabel);
  }

  encoder.unsignedNumber(metadata.files.size());
  for (const auto &file : metadata.files) {
    encoder.text(file.category);
    encoder.text(file.name);
    encoder.text(file.pattern);
    encoder.text(file.defaultValue);
    encoder.unsignedNumber(file.choices.size());
    for (const auto &choice : file.choices) {
      encoder.text(choice);
    }
  }

  encoder.unsignedNumber(metadata.offsets.size());
  for (const auto &offset : metadata.offsets) {
    encoder.text(offset.category);
    encoder.text(offset.name);
    encoder.signedNumber(offset.id);
    encoder.unsignedNumber(offset.permissions);
  }
}

void encodeViewport(PresentationKeyEncoder &encoder,
                    const ViewportSettings &viewport) {
  encodeEnum(encoder, viewport.mode);
  encodeEnum(encoder, viewport.customBase);
  encoder.floatingPoint(viewport.scaleX);
  encoder.floatingPoint(viewport.scaleY);
  encoder.floatingPoint(viewport.translateX);
  encoder.floatingPoint(viewport.translateY);
}

void encodeSettings(PresentationKeyEncoder &encoder,
                    const EntryProfileSettings &settings) {
  encoder.unsignedNumber(settings.options.size());
  for (const auto &[name, value] : settings.options) {
    encoder.text(name);
    encoder.signedNumber(value);
  }

  encoder.unsignedNumber(settings.filePaths.size());
  for (const auto &[name, value] : settings.filePaths) {
    encoder.text(name);
    encoder.text(value);
  }

  encoder.unsignedNumber(settings.offsets.size());
  for (const auto &[name, value] : settings.offsets) {
    encoder.text(name);
    encoder.signedNumber(value.x);
    encoder.signedNumber(value.y);
    encoder.signedNumber(value.w);
    encoder.signedNumber(value.h);
    encoder.signedNumber(value.r);
    encoder.signedNumber(value.a);
  }
  encodeViewport(encoder, settings.viewport);
}

} // namespace

GameplaySkinSettingsActionAvailability gameplaySkinSettingsActionAvailability(
    const GameplaySkinSettingsSnapshot &snapshot) noexcept {
  const bool nameReady = snapshot.state == GameplaySkinSettingsState::Ready &&
                         snapshot.canCancel &&
                         snapshot.preparedName.has_value();
  return {
      .ordinaryActions = snapshot.state != GameplaySkinSettingsState::Busy &&
                         !snapshot.canCancel,
      .canCancel = snapshot.canCancel,
      .canEditPreparedName = nameReady,
      .canInstallPrepared = nameReady && snapshot.preparedName->ok(),
  };
}

std::string gameplaySkinSettingsPresentationKey(
    const GameplaySkinSettingsSnapshot &snapshot) {
  PresentationKeyEncoder encoder;
  encodeEnum(encoder, snapshot.state);
  encoder.boolean(snapshot.featureAvailable);
  encoder.boolean(snapshot.compatibilityEnabled);
  encoder.text(snapshot.statusMessage);
  encoder.boolean(snapshot.canCancel);

  encodeEnum(encoder, snapshot.progress.phase);
  encoder.unsignedNumber(snapshot.progress.completedBytes);
  encoder.unsignedNumber(snapshot.progress.totalBytes);
  encoder.unsignedNumber(snapshot.progress.completedFiles);

  encoder.unsignedNumber(snapshot.selectedGameplayEntries.size());
  for (const auto &[skinType, entry] : snapshot.selectedGameplayEntries) {
    encoder.signedNumber(skinType);
    encodeEntry(encoder, entry);
  }

  encoder.boolean(snapshot.preparedName.has_value());
  if (snapshot.preparedName) {
    encoder.text(snapshot.preparedName->originalSourceName);
    encoder.text(snapshot.preparedName->suggestedPackageName);
    encoder.text(snapshot.preparedName->validationError);
  }

  encoder.boolean(snapshot.collisionPackage.has_value());
  if (snapshot.collisionPackage) {
    encodePackage(encoder, *snapshot.collisionPackage);
  }

  encoder.unsignedNumber(snapshot.entries.size());
  for (const auto &row : snapshot.entries) {
    encodeEntry(encoder, row.entry);
    encodeMetadata(encoder, row.metadata);
    encoder.text(row.revisionDigest);
    encoder.text(row.configurationDigest);
    encodeEnum(encoder, row.validation);
    encodeSettings(encoder, row.settings);
    encoder.unsignedNumber(row.diagnostics.size());
    for (const auto &diagnostic : row.diagnostics) {
      encodeDiagnostic(encoder, diagnostic);
    }
  }

  encoder.unsignedNumber(snapshot.history.size());
  for (const auto &record : snapshot.history) {
    encoder.unsignedNumber(record.recordSerial);
    encodeEntry(encoder, record.entry);
    encoder.text(record.revisionDigest);
    encoder.text(record.configurationDigest);
    encodeEnum(encoder, record.phase);
    encodeDiagnostic(encoder, record.diagnostic);
    encoder.boolean(record.luaLine.has_value());
    if (record.luaLine) {
      encoder.unsignedNumber(*record.luaLine);
    }
    encoder.boolean(record.frameSerial.has_value());
    if (record.frameSerial) {
      encoder.unsignedNumber(*record.frameSerial);
    }
  }
  return std::move(encoder).finish();
}

ViewportSettings gameplaySkinViewportWithMode(ViewportSettings current,
                                              ViewportMode mode) noexcept {
  current.mode = mode;
  return current;
}

ViewportSettings
gameplaySkinViewportWithCustomBase(ViewportSettings current,
                                   CustomViewportBase customBase) noexcept {
  current.mode = ViewportMode::Custom;
  current.customBase = customBase;
  return current;
}

} // namespace skin
