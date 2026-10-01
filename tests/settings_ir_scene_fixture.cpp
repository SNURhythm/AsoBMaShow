// Rendering is replaced; retained field, click handlers, result publisher and
// success arguments are extracted directly from the production scene.
#include "i18n/Localization.h"
#include "ir/IrSettingsPresentation.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>

namespace {
void require(bool value, const char *reason) {
  if (!value) throw std::runtime_error(reason);
}
struct Button {
  std::function<void()> click;
  void setOnClickListener(std::function<void()> value) { click = std::move(value); }
};
struct Input {
  std::string value = "secret";
  std::string getText() { return value; }
  void setEditingText(std::string text) { value = std::move(text); }
};
struct SettingsScene {
  // STATUS_FIELD
  bool irStatusIsError = false;
  bool irKeyEditorActive = true;
  int lastLayoutWidth = 800;
  std::optional<std::int64_t> irPendingDiscardRowId;
  Input input;
  Input *irApiKeyInput = &input;
  Button cancelButton, replaceButton, discardButton;
  void bind() {
    auto *cancelKey = &cancelButton;
    auto *replaceKey = &replaceButton;
    auto *discard = &discardButton;
    struct { std::int64_t rowId = 42; } snapshot;
    // CALLBACK_cancelKey
    // CALLBACK_replaceKey
    // CALLBACK_discard
  }
  void verifyRetained(const char *key) {
    i18n::setLanguage(i18n::Language::English);
    require(i18n::Text(irStatusMessage).resolve() == i18n::tr(key), "initial status must match action");
    // Rebuilding the IR tab copies its retained state into a new bound view.
    const i18n::Text rebuilt(irStatusMessage);
    for (auto language : {i18n::Language::Korean, i18n::Language::Japanese}) {
      i18n::setLanguage(language);
      require(rebuilt.resolve() == i18n::tr(key), "retained IR status must follow language changes");
    }
    i18n::setLanguage(i18n::Language::English);
  }
  void testResultPublication() {
    // PUBLISH_RESULT
    ir::IrSettingsActionResult result{.status = ir::IrSettingsActionResult::Status::Succeeded};
    // SUCCESS_CASES
    require(!irStatusIsError && lastLayoutWidth == -1, "successful actions refresh non-error status");
    result.status = ir::IrSettingsActionResult::Status::StorageFailure;
    publishResult(result, "unused success");
    verifyRetained("settings.ir.ir_setting_failed_changed.message");
    require(irStatusIsError, "failure status is marked as an error");
    const std::string diagnostic = "Server: 日本語 한국어 /tmp/IR";
    result.diagnostic = diagnostic;
    publishResult(result, "unused success");
    result.diagnostic.clear();
    i18n::setLanguage(i18n::Language::Korean);
    require(i18n::Text(irStatusMessage).resolve() == diagnostic,
            "backend diagnostic must remain owned verbatim text");
  }
};
}
int main() {
  i18n::setLanguage(i18n::Language::English);
  SettingsScene scene;
  scene.bind();
  scene.cancelButton.click();
  require(scene.input.value.empty() && !scene.irKeyEditorActive,
          "cancel must clear credential editor");
  scene.verifyRetained("settings.ir.api_key_edit_cancelled.message");
  scene.discardButton.click();
  require(scene.irPendingDiscardRowId == 42 && scene.irStatusIsError,
          "discard confirmation retains row identity and warning");
  scene.verifyRetained("settings.ir.confirm_permanent_removal_queued_score.message");
  scene.replaceButton.click();
  require(scene.irStatusMessage.empty() && scene.irKeyEditorActive,
          "new credential edit clears old status");
  scene.testResultPublication();
  i18n::setLanguage(i18n::Language::English);
}
