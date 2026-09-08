#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

struct MusicSelectRepositoryMetadata {
  struct Entry { std::filesystem::path path; };
  struct Folder { std::string path; int addDateSeconds; };
  std::vector<Entry> entries;
  std::vector<Folder> folders;
};
MusicSelectRepositoryMetadata projectedMetadata;
struct MusicSelectRepositoryProjection {
  int project(const MusicSelectRepositoryMetadata &metadata) {
    projectedMetadata = metadata;
    return 0;
  }
};
std::filesystem::path fspath_to_path_t(const std::filesystem::path &path) { return path; }
int musicSelectProjectionChildren(int, const std::string &) { return 0; }
int main() {
  MusicSelectRepositoryMetadata repositoryMetadata_;
  repositoryMetadata_.folders = {{"category/empty", 123},
                                  {"category/populated", 456}};
  struct Directory { std::filesystem::path directoryPath; std::string id; };
  const Directory directory{"category", "folder:category"};
  const std::vector<int> records{1};
  auto inputFor = [](const auto &, const MusicSelectRepositoryMetadata *metadata) {
    return *metadata;
  };
  int children = 0;
  do SCENE_FOLDER_BRANCH while (false);
  assert(projectedMetadata.entries.size() == 1);
  assert(projectedMetadata.entries.front().path == "category");
  assert(projectedMetadata.folders.size() == 2 &&
         "recursive chart queries must not hide persisted empty category children");
  assert(projectedMetadata.folders.front().path == "category/empty");
  assert(projectedMetadata.folders.front().addDateSeconds == 123);
}
