#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef ASOBMASHOW_SOURCE_DIR
#error "ASOBMASHOW_SOURCE_DIR must identify the repository root"
#endif

namespace {

int failures = 0;

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string_view functionBody(std::string_view source,
                              std::string_view signature) {
  const std::size_t begin = source.find(signature);
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t open = source.find('{', begin + signature.size());
  if (open == std::string_view::npos) {
    return {};
  }
  int depth = 0;
  for (std::size_t index = open; index < source.size(); ++index) {
    if (source[index] == '{') {
      ++depth;
    } else if (source[index] == '}' && --depth == 0) {
      return source.substr(open, index - open + 1);
    }
  }
  return {};
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testCompletedScanInvalidatesOnlyTheMemoryHotPath() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const std::string menu = readText(root / "src/scene/MainMenuScene.cpp");
  const std::string image = readText(root / "src/view/ImageView.cpp");

  const std::string_view scan = functionBody(
      menu, "void MainMenuScene::LoadCharts(ChartRepository::Session");
  require(scan.contains("scene.requestLibraryReload(true)") &&
              !scan.contains("if (changedCount > 0)"),
          "every completed library scan enters the shared reload boundary "
          "even when chart metadata did not change");

  const std::string_view apply =
      functionBody(menu, "void MainMenuScene::applyPendingUiUpdates()");
  require(apply.contains("ImageView::dropAllCache()"),
          "the shared UI-thread reload boundary clears decoded and failed "
          "artwork entries");

  const std::string_view revision =
      functionBody(menu, "void MainMenuScene::refreshLibraryIfNeeded()");
  require(revision.contains("ImageView::dropAllCache()"),
          "repository revision refreshes use the same artwork invalidation "
          "policy");

  require(!image.contains("last_write_time") && !image.contains("file_size"),
          "ordinary artwork cache hits do not add filesystem metadata probes");
  require(!image.contains("asobmashow-file-thumb"),
          "ordinary artwork does not regain a disk preview cache");
}

void testFolderSelectionPrioritizesTheNewVisibleArtworkBatch() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const std::string menu = readText(root / "src/scene/MainMenuScene.cpp");
  const std::string item = readText(root / "src/view/ChartListItemView.cpp");

  const std::string_view selectFolder =
      functionBody(menu, "void MainMenuScene::selectFolder(");
  require(selectFolder.contains("reloadChartListForFolderSelection()"),
          "folder selection marks its newly visible chart batch as priority");
  const std::string_view prioritizedReload = functionBody(
      menu, "void MainMenuScene::reloadChartListForFolderSelection()");
  require(prioritizedReload.contains(
              "prioritizeVisibleArtworkBindings = true") &&
              prioritizedReload.contains("reloadChartList()") &&
              prioritizedReload.contains(
                  "prioritizeVisibleArtworkBindings = false"),
          "folder priority is scoped to the synchronous visible-row rebind");
  require(menu.contains(
              "setMeta(item, prioritizeVisibleArtworkBindings)"),
          "the recycler binding forwards folder priority to each visible row");
  require(item.contains("setImageAsync(meta.Folder / meta.StageFile,") &&
              item.contains("setImageAsync(meta.Folder / meta.Banner,") &&
              item.contains("bool prioritizeArtwork"),
          "both jackets and banners share the visible-row priority policy");
}

} // namespace

int main() {
  testCompletedScanInvalidatesOnlyTheMemoryHotPath();
  testFolderSelectionPrioritizesTheNewVisibleArtworkBatch();
  return failures == 0 ? 0 : 1;
}
