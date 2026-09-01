#pragma once

#include "GameplaySkinDocumentLoader.h"
#include "LuaSkinAudioHost.h"
#include "MusicSelectSkinStateBridge.h"
#include "Skin2DRenderer.h"
#include "SkinMovieCatalog.h"
#include "SkinResourceCatalog.h"

#include "../GameplaySkinActivationRequest.h"
#include "../SkinStoragePaths.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

struct RenderContext;

namespace rendering {
class SkinQuadBatchRenderer;
}

namespace skin {

struct MusicSelectSkinSessionContext {
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  MusicSelectSkinFrame initialFrame;
  std::shared_ptr<SkinTextureDevice> textureDevice;
  std::shared_ptr<SkinMovieDevice> movieDevice;
  SkinBuiltinImageReader builtinImageReader;
  std::shared_ptr<LuaSkinAudioBackend> audioBackend;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};

struct MusicSelectSkinSessionCreateResult {
  std::unique_ptr<class MusicSelectSkinSession> session;
  std::vector<SkinDiagnostic> diagnostics;
};

enum class MusicSelectSkinActionKind : std::uint8_t {
  Event,
  FloatWriter,
  StringWriter,
};

struct MusicSelectSkinAction {
  MusicSelectSkinActionKind kind = MusicSelectSkinActionKind::Event;
  SkinBuiltinPropertySelector selector;
  std::vector<int> arguments;
  double floatValue = 0.0;
  std::string stringValue;
};

struct MusicSelectSkinPointerResult {
  bool consumed = false;
  std::optional<std::size_t> selectIndex;
  bool closeDirectory = false;
  std::optional<PresentationUiHit> capturedControl;
  std::optional<SkinStringWriterId> focusedStringWriter;
};

class MusicSelectSkinSession final {
public:
  static MusicSelectSkinSessionCreateResult
  create(GameplaySkinActivationRequest, MusicSelectSkinSessionContext);
  ~MusicSelectSkinSession();

  MusicSelectSkinSession(const MusicSelectSkinSession &) = delete;
  MusicSelectSkinSession &operator=(const MusicSelectSkinSession &) = delete;

  [[nodiscard]] bool render(RenderContext &, const MusicSelectSkinFrame &);
  [[nodiscard]] bool
  requiresResourceRefresh(const MusicSelectSkinFrame &) const;
  [[nodiscard]] bool refreshResources(const MusicSelectSkinFrame &);
  [[nodiscard]] MusicSelectSkinPointerResult
  queuePointerDown(UiLogicalPoint, int button, long long eventMicros);
  [[nodiscard]] bool queuePointerMove(const PresentationUiHit &,
                                      UiLogicalPoint, long long eventMicros);
  [[nodiscard]] bool queueStringWrite(SkinStringWriterId, std::string);
  [[nodiscard]] std::vector<MusicSelectSkinAction> takePublishedActions();
  [[nodiscard]] std::vector<SkinDiagnostic> takeLastDiagnostics();
  [[nodiscard]] const SkinEntryId &entry() const noexcept;

private:
  struct QueuedEventBinding {
    SkinEventBindingId binding{};
    std::array<int, 2> arguments{};
    std::size_t argumentCount = 0;
  };
  struct QueuedFloatWriter {
    SkinFloatWriterId writer{};
    double value = 0.0;
  };
  struct QueuedStringWriter {
    SkinStringWriterId writer{};
    std::string value;
  };

  MusicSelectSkinSession(
      std::uint64_t, SkinProfileId, SkinRevisionLease, SkinEntryId,
      ValidatedBeatorajaSkinModel, BeatorajaSkinConfiguration,
      std::unique_ptr<LuaSkinRuntime>, std::unique_ptr<SkinResourceCatalog>,
      std::unique_ptr<SkinMovieCatalog>, SkinStorageRoots,
      SkinResourcePreparationService &, std::shared_ptr<SkinTextureDevice>,
      SkinBuiltinImageReader, std::shared_ptr<SkinLiveResourceCounters>,
      SkinSafetyPolicy, ViewportSettings, std::stop_token,
      std::vector<std::string>, std::map<int, std::filesystem::path>);

  static LuaSkinEventExecutionResult executeHostEvent(
      void *, int, std::span<const int>) noexcept;
  [[nodiscard]] bool queueEvent(int, std::span<const int>);
  [[nodiscard]] bool queueBuiltinEvent(SkinBuiltinPropertySelector,
                                       std::span<const int>);
  [[nodiscard]] bool queueEventBinding(SkinEventBindingId,
                                       std::span<const int>);
  [[nodiscard]] bool queueFloatWriter(const SkinWriterInvocation &);
  [[nodiscard]] bool executeQueuedCallbacks(MusicSelectSkinStateBridge &);

  std::uint64_t sessionSerial_ = 0;
  SkinProfileId profileId_;
  SkinRevisionLease revision_;
  SkinEntryId entry_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::unique_ptr<SkinResourceCatalog> resources_;
  std::unique_ptr<SkinMovieCatalog> movies_;
  SkinStorageRoots storageRoots_;
  SkinResourcePreparationService *resourcePreparation_ = nullptr;
  std::shared_ptr<SkinTextureDevice> textureDevice_;
  SkinBuiltinImageReader builtinImageReader_;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters_;
  SkinSafetyPolicy safetyPolicy_{};
  ViewportSettings viewportSettings_{};
  std::stop_token stop_;
  Skin2DRenderer renderer_;
  std::unique_ptr<rendering::SkinQuadBatchRenderer> quadRenderer_;
  std::optional<SkinInteractionLayout> publishedInteractionLayout_;
  std::vector<QueuedEventBinding> queuedEvents_;
  std::vector<QueuedFloatWriter> queuedFloatWriters_;
  std::vector<QueuedStringWriter> queuedStringWriters_;
  std::vector<MusicSelectSkinAction> frameActions_;
  std::vector<MusicSelectSkinAction> queuedActions_;
  std::vector<MusicSelectSkinAction> publishedActions_;
  std::vector<SkinDiagnostic> diagnostics_;
  std::map<int, std::size_t> customEventLastDefinitionIndexes_;
  std::map<int, std::size_t> customTimerLastDefinitionIndexes_;
  std::map<int, std::int64_t> customEventLastExecutionMicros_;
  std::vector<std::string> preparedRuntimeStrings_;
  std::map<int, std::filesystem::path> preparedBuiltinImagePaths_;
  std::int64_t currentEventMicros_ = 0;
  bool executingFrame_ = false;
};

} // namespace skin
