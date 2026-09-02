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
#include <future>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <stop_token>
#include <unordered_map>
#include <vector>

struct RenderContext;

namespace rendering {
class SkinQuadBatchRenderer;
class SkinQuadBatchBackend;
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
  rendering::SkinQuadBatchBackend *quadBackend = nullptr;
  std::function<LuaSkinLegacyInputGeneration()> captureLegacyInputGeneration;
  std::stop_token stop;
};

[[nodiscard]] constexpr SkinSafetyPolicy
musicSelectSkinCompatibilityPolicy() noexcept {
  // Pinned LuaSkinLoader selects SkinLuaAccessor(false), whose standard
  // globals include SafeOsLib.  The sandbox constructor is a separate,
  // explicitly requested loader mode and is not the type-5 selector path.
  return SkinSafetyPolicy(SkinSafetyLevel::BeatorajaCompatibility);
}

// JSONSkinLoader chooses a source Resolution by exact width/height match and
// otherwise retains its HD default before constructing the selector Skin.
[[nodiscard]] AuthoredSize
musicSelectSkinSourceResolution(const BeatorajaSkinHeader &header) noexcept;

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

// Loading a type-5 skin has two ownership domains. Lua/document decoding and
// CPU resource planning may run before the selector's render loop starts;
// movies and texture uploads must remain on the render-owner thread.
struct MusicSelectSkinSessionPreparationContext {
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  MusicSelectSkinFrame initialFrame;
  SkinBuiltinImageReader builtinImageReader;
  std::shared_ptr<LuaSkinAudioBackend> audioBackend;
  std::optional<LuaSkinLegacyInputGeneration> initialLegacyInputGeneration;
  std::stop_token stop;
};

struct MusicSelectSkinSessionPrepared {
  GameplaySkinActivationRequest request;
  SkinStorageRoots storageRoots;
  LoadedGameplaySkinDocument document;
  SkinResourceUploadPlan resourcePlan;
  std::map<SkinObjectId, std::vector<std::string>> runtimeAtlasStrings;
  std::map<int, std::filesystem::path> builtinImagePaths;
  std::vector<MusicSelectSkinAction> initialActions;
  SkinBuiltinImageReader builtinImageReader;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};

struct MusicSelectSkinSessionPreparationResult {
  std::optional<MusicSelectSkinSessionPrepared> prepared;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

struct MusicSelectSkinSessionFinalizationContext {
  SkinResourcePreparationService &resourcePreparation;
  std::shared_ptr<SkinTextureDevice> textureDevice;
  std::shared_ptr<SkinMovieDevice> movieDevice;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters;
  rendering::SkinQuadBatchBackend *quadBackend = nullptr;
  std::function<LuaSkinLegacyInputGeneration()> captureLegacyInputGeneration;
};

struct MusicSelectSkinPointerResult {
  struct StringFocus {
    SkinStringWriterId writer{};
    std::string currentValue;
    UiLogicalRect bounds;
    std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
  };

  bool consumed = false;
  std::optional<std::size_t> selectIndex;
  bool closeDirectory = false;
  std::optional<StringFocus> focusedStringWriter;
};

enum class MusicSelectSkinPointerTargetKind : std::uint8_t {
  None,
  Bar,
  Slider,
  Image,
  Text,
};

struct MusicSelectSkinPointerTarget {
  MusicSelectSkinPointerTargetKind kind =
      MusicSelectSkinPointerTargetKind::None;
  std::optional<std::size_t> selectIndex;
};

struct MusicSelectBuiltinImagePatch {
  std::map<int, std::filesystem::path> paths;
  std::map<int, std::optional<image_decode::DecodedImageData>> images;
};

struct MusicSelectTextAtlasPatch {
  std::map<SkinObjectId, std::vector<std::string>> runtimeStrings;
  std::vector<SkinPreparedTextAtlasUpdate> atlases;
  std::vector<SkinDiagnostic> diagnostics;
  bool cancelled = false;
};

class MusicSelectSkinSession final {
public:
  static MusicSelectSkinSessionPreparationResult
  prepare(GameplaySkinActivationRequest,
          MusicSelectSkinSessionPreparationContext);
  static MusicSelectSkinSessionCreateResult
  finalize(MusicSelectSkinSessionPrepared,
           MusicSelectSkinSessionFinalizationContext);
  static MusicSelectSkinSessionCreateResult
  create(GameplaySkinActivationRequest, MusicSelectSkinSessionContext);
  ~MusicSelectSkinSession();

  MusicSelectSkinSession(const MusicSelectSkinSession &) = delete;
  MusicSelectSkinSession &operator=(const MusicSelectSkinSession &) = delete;

  [[nodiscard]] bool render(RenderContext &, const MusicSelectSkinFrame &);
  [[nodiscard]] bool
  requiresResourceRefresh(const MusicSelectSkinFrame &) const;
  [[nodiscard]] bool refreshResources(const MusicSelectSkinFrame &);
  void suspendAudio() noexcept;
  void resumeAudio() noexcept;
  [[nodiscard]] MusicSelectSkinPointerTarget
  pointerTargetAt(UiLogicalPoint) const noexcept;
  [[nodiscard]] MusicSelectSkinPointerResult
  queuePointerDown(UiLogicalPoint, int button, long long eventMicros);
  [[nodiscard]] bool queuePointerDrag(UiLogicalPoint, long long eventMicros);
  [[nodiscard]] bool queueStringWrite(SkinStringWriterId, std::string);
  [[nodiscard]] std::vector<MusicSelectSkinAction> takePublishedActions();
  [[nodiscard]] std::vector<SkinDiagnostic> takeLastDiagnostics();
  [[nodiscard]] const SkinEntryId &entry() const noexcept;
  [[nodiscard]] int inputDelayMillis() const noexcept;

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
      rendering::SkinQuadBatchBackend *, SkinSafetyPolicy, ViewportSettings,
      std::stop_token,
      std::map<SkinObjectId, std::vector<std::string>>,
      std::map<int, std::filesystem::path>);

  static LuaSkinEventExecutionResult executeHostEvent(
      void *, int, std::span<const int>) noexcept;
  [[nodiscard]] bool queueEvent(int, std::span<const int>,
                                std::span<const int> resolutionPath = {});
  [[nodiscard]] bool queueBuiltinEvent(SkinBuiltinPropertySelector,
                                       std::span<const int>);
  [[nodiscard]] bool queueEventBinding(SkinEventBindingId,
                                       std::span<const int>,
                                       std::span<const int> resolutionPath = {});
  [[nodiscard]] bool queueFloatWriter(const SkinWriterInvocation &);
  [[nodiscard]] bool executeQueuedCallbacks(MusicSelectSkinStateBridge &);
  void updateBuiltinImages(const MusicSelectSkinFrame &);
  [[nodiscard]] bool
  updateRuntimeTextAtlases(const MusicSelectSkinFrame &);

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
  std::map<int, std::int64_t> customTimerValues_;
  std::set<int> activeCustomTimerIds_;
  std::map<int, std::int64_t> customEventLastExecutionMicros_;
  std::map<SkinObjectId, std::vector<std::string>>
      preparedRuntimeStringsByObject_;
  std::map<SkinObjectId, std::vector<std::string>>
      observedRuntimeStringsByObject_;
  std::map<int, std::filesystem::path> preparedBuiltinImagePaths_;
  std::map<int, std::filesystem::path> pendingBuiltinImagePaths_;
  std::stop_source builtinImagePatchStop_;
  std::future<MusicSelectBuiltinImagePatch> pendingBuiltinImagePatch_;
  std::map<SkinObjectId, std::vector<std::string>>
      pendingRuntimeStringsByObject_;
  std::set<SkinObjectId> pendingTextAtlasObjects_;
  std::set<SkinObjectId> unavailableTextAtlasObjects_;
  std::stop_source textAtlasPatchStop_;
  std::future<MusicSelectTextAtlasPatch> pendingTextAtlasPatch_;
  std::function<LuaSkinLegacyInputGeneration()> captureLegacyInputGeneration_;
  std::int64_t currentEventMicros_ = 0;
  bool executingFrame_ = false;
};

} // namespace skin
