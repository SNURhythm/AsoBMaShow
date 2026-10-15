"""Run retained utility-tab resumes with counted data/credential reads."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class UtilityTabResumeTests(unittest.TestCase):
    def test_music_service_invalidates_snapshots_after_content_refreshes(self):
        root = Path(__file__).resolve().parents[1]
        service = (root / "src/audio/MusicPlayerService.cpp").read_text()
        source = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace bms_parser { struct ChartMeta {}; }
using MusicTrackRecord=int;
namespace music_playlist {
using MusicTrack=int;
std::vector<int> MakeTracks(const std::vector<int> &rows) { return rows; }
}
constexpr auto kDefaultPlaylistName="default";
struct Playlist { int id; };
struct Repository {
  int EnsurePlaylist(const char*) { return 1; }
  std::vector<Playlist> SelectPlaylists() { return {{1},{2}}; }
  std::vector<int> SelectLibraryTracks() { return {3,4}; }
  struct State { int selectedPlaylistId=1; };
  State SelectPlayerState() { return {}; }
};
struct Charts {
  struct Session { bool SetFavorite(const bms_parser::ChartMeta&,bool) { return true; } };
  std::optional<Session> OpenSession() { return Session{}; }
};
bool hasPlaylistId(const std::vector<Playlist>&,int id) { return id==1 || id==2; }
std::vector<int> loadPlaylistTracks(Repository&,int id) { return {id}; }
std::vector<int> loadFavoriteTracks(Charts&) { return {4}; }
struct MusicPlayerService {
  Repository repository;
  Charts charts;
  std::mutex stateMutex;
  std::atomic<std::uint64_t> contentRevision{0};
  std::vector<int> libraryTracks,favoriteTracks,defaultPlaylistTracks,selectedPlaylistTracks;
  std::vector<Playlist> playlists;
  Repository::State persistedState;
  int selectedPlaylistId=0,defaultPlaylistId=0;
  bool ready=true;
  bool EnsureRepositoryReadyLocked(std::string&) { return ready; }
  void RestoreQueueFromPersistedStateLocked() {}
  bool ReloadLibrary(std::string&);
  bool ReloadLibraryAndPlaylists(std::string&,int);
  bool SetFavorite(const bms_parser::ChartMeta&,bool,std::string&);
  void RefreshPlaylistCachesLocked(int);
};
METHODS
int main() {
  MusicPlayerService service;
  std::string error;
  assert(service.ReloadLibrary(error));assert(service.contentRevision==1);
  assert(service.SetFavorite({},true,error));assert(service.contentRevision==2);
  service.RefreshPlaylistCachesLocked(2);assert(service.contentRevision==3);
  assert(service.selectedPlaylistId==2);
  assert(service.ReloadLibraryAndPlaylists(error,1));assert(service.contentRevision==4);
  service.ready=false;
  assert(!service.ReloadLibrary(error));assert(service.contentRevision==4);
  assert(!service.ReloadLibraryAndPlaylists(error,1));assert(service.contentRevision==4);
}
'''
        methods = "\n".join(extract(service, signature) for signature in (
            "bool MusicPlayerService::ReloadLibrary(",
            "bool MusicPlayerService::ReloadLibraryAndPlaylists(",
            "bool MusicPlayerService::SetFavorite(",
            "void MusicPlayerService::RefreshPlaylistCachesLocked(",
        ))
        self.compile_and_run(source.replace("METHODS", methods))

    def compile_and_run(self, source):
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "resume.cpp"
            binary = Path(directory) / ("resume" + compiler.executable_suffix)
            path.write_text(source)
            compiler.build([path], binary, directory)
            subprocess.run([str(binary)], check=True)

    def test_unchanged_tabs_reuse_data_and_changes_invalidate_it(self):
        root = Path(__file__).resolve().parents[1]
        music = (root / "src/scene/MusicPlayerScene.cpp").read_text()
        ir = (root / "src/scene/IrUploadsScene.cpp").read_text()
        source = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace rendering { int window_width=1920,window_height=1080; }
struct Insets { int top=0,left=0,bottom=0,right=0; };
Insets getSafeAreaInsetsUi() { return {}; }
namespace ir { constexpr auto kTachiProviderId="tachi"; }
struct Context {
  struct ProfileManager {
    struct Profile { std::string id="one"; } profile;
    const Profile &activeProfile() const { return profile; }
  } profileManager;
  struct Repository {
    std::uint64_t revision=1;
    std::uint64_t GetLibraryRevision() const { return revision; }
    std::uint64_t GetRevision() const { return revision; }
    std::uint64_t ContentRevision() const { return revision; }
  } chartRepository,scoreRepository,musicPlayer;
  std::atomic<std::uint64_t> profileCacheRevision{1}, irAccountEvidenceRevision{1};
  struct Submission {
    struct Status { std::uint64_t revision=1; } value;
    Status status(std::string_view,std::string_view) const { return value; }
  } submission;
  Submission *irSubmissionService=&submission;
};
struct MusicPlayerScene {
  Context context;
  std::string displayedProfileId="one",statusMessage;
  int pendingDeletePlaylistId=0,pendingClearPlaylistId=0,selectedLibraryPlaylistId=0;
  bool dataLoaded=true;
  std::uint64_t loadedLibraryRevision=1,loadedProfileCacheRevision=1,loadedContentRevision=1;
  int reads=0,queueRefreshes=0,metadataPublishes=0;
  bool preserved=false;
  void onResume();
  void applySystemPlaybackPrivacy(bool) { ++metadataPublishes; }
  void refreshSystemPlaybackPrivacyButtons() {}
  void reloadData(bool preserve) {
    ++reads; preserved=preserve; dataLoaded=true;
    loadedLibraryRevision=context.chartRepository.GetLibraryRevision();
    loadedProfileCacheRevision=context.profileCacheRevision.load();
    loadedContentRevision=context.musicPlayer.ContentRevision();
  }
  void refreshActiveQueueList(bool) { ++queueRefreshes; }
  void refreshUi() {}
  void refreshLanguageIfNeeded() {}
  void updateLayoutInsets() {}
};
struct IrUploadsScene {
  Context context;
  std::string displayedProfileId="one",origin="https://example.test";
  std::string observedServerOrigin="https://example.test";
  std::uint64_t observedScoreRevision=1,observedProfileCacheRevision=1;
  std::uint64_t observedAccountEvidenceRevision=1,observedAttemptStatusRevision=1;
  bool candidatesLoaded=true,reloadRequested=false;
  bool presentationActive=true;
  struct Controller {
    bool locked=false;
    int prepared=0;
    bool selectionLocked() const { return locked; }
    void setPreparationProgress(int completed,int) { prepared=completed; }
    void completePreparation(int) { locked=false; }
  } controller;
  struct PreparationTask {
    struct Updates {
      std::optional<std::pair<int,int>> progress;
      std::optional<int> completion;
    } pending;
    Updates takeUpdates() { return std::exchange(pending,{}); }
  } preparationTask;
  int reads=0,credentials=0,uiRefreshes=0,pointerCancellations=0,layoutUpdates=0;
  int *rootLayout=nullptr;
  int lastLayoutWidth=1920,lastLayoutHeight=1080;
  int lastSafeTop=0,lastSafeLeft=0,lastSafeBottom=0,lastSafeRight=0;
  void onPause();
  void onResume();
  void update(float);
  void applyPreparationUpdates();
  void observeRemoteRevisions();
  void refreshProfileIfNeeded();
  void onProfileWillChange() { candidatesLoaded=false;reloadRequested=false;displayedProfileId.clear(); }
  void reloadCandidates() {
    ++reads;refreshProviderState();reloadRequested=false;candidatesLoaded=true;
    observeRemoteRevisions();reloadRequested=false;
    refreshUi();
  }
  void refreshProviderState() { ++credentials; }
  void refreshUi() { ++uiRefreshes; }
  void onInputQueueOverflow() { ++pointerCancellations; }
  void refreshLanguageIfNeeded() {}
  void updateLayoutInsets() { ++layoutUpdates; }
  std::string serverOrigin() const { return origin; }
};
MUSIC_RESUME
IR_RESUME
IR_PROFILE
IR_OBSERVE
IR_PAUSE
IR_UPDATE
IR_PREPARATION
int main() {
  MusicPlayerScene music;
  for(int i=0;i<20;++i) music.onResume();
  assert(music.reads==0 && music.queueRefreshes==20 && music.metadataPublishes==0);
  ++music.context.chartRepository.revision;music.onResume();
  assert(music.reads==1 && music.preserved);
  ++music.context.musicPlayer.revision;music.onResume();
  assert(music.reads==2);
  ++music.context.profileCacheRevision;music.onResume();
  assert(music.reads==3);
  music.context.profileManager.profile.id="two";music.onResume();
  assert(music.reads==4 && !music.preserved);
  music.dataLoaded=false;music.onResume();
  assert(music.reads==5);
  music.onResume();assert(music.reads==5);

  IrUploadsScene ir;
  for(int i=0;i<20;++i) ir.onResume();
  assert(ir.reads==0 && ir.credentials==0);
  ++ir.context.scoreRepository.revision;ir.onResume();
  assert(ir.reads==1);
  ++ir.context.irAccountEvidenceRevision;ir.onResume();
  assert(ir.reads==2);
  ++ir.context.submission.value.revision;ir.onResume();
  assert(ir.reads==3);
  ++ir.context.profileCacheRevision;ir.onResume();
  assert(ir.reads==4);
  ir.origin="https://changed.test";ir.onResume();assert(ir.reads==5);
  ir.context.profileManager.profile.id="two";ir.onResume();assert(ir.reads==6);
  ir.onResume();assert(ir.reads==6);
  ir.controller.locked=true;
  ++ir.context.scoreRepository.revision;ir.onResume();
  assert(ir.reads==6 && ir.reloadRequested);
  ir.controller.locked=false;ir.onResume();assert(ir.reads==7);
  ir.candidatesLoaded=false;ir.onResume();assert(ir.reads==8);
  assert(ir.credentials==ir.reads);

  IrUploadsScene hidden;
  ++hidden.context.submission.value.revision;
  hidden.update(0);
  assert(hidden.reads==1 && "a standalone foreground scene must refresh normally");
  hidden.onPause();
  assert(hidden.pointerCancellations==1);
  const int visibleRefreshes=hidden.uiRefreshes;
  for(int frame=0;frame<600;++frame) {
    if(frame%60==0) ++hidden.context.submission.value.revision;
    hidden.update(0);
  }
  assert(hidden.reads==1 && hidden.credentials==1 &&
         "hidden IR delivery revisions must not rescan candidate history");
  hidden.controller.locked=true;
  hidden.preparationTask.pending.progress=std::pair{1,2};
  hidden.update(0);
  assert(hidden.controller.prepared==1 && hidden.controller.locked);
  hidden.preparationTask.pending.completion=1;
  hidden.update(0);
  assert(!hidden.controller.locked && hidden.reloadRequested &&
         "hidden preparation completion must still reach the controller");
  assert(hidden.reads==1 && hidden.uiRefreshes==visibleRefreshes &&
         hidden.layoutUpdates==0 && "hidden completion must defer presentation work");
  hidden.onResume();
  assert(hidden.reads==2 && hidden.credentials==2 && !hidden.reloadRequested &&
         "resume must coalesce hidden revisions and completion into one refresh");
  hidden.update(0);hidden.onResume();
  assert(hidden.reads==2);

  hidden.onPause();
  hidden.controller.locked=true;
  hidden.preparationTask.pending.progress=std::pair{2,3};
  ++hidden.context.scoreRepository.revision;
  hidden.update(0);
  const int beforeResume=hidden.uiRefreshes;
  hidden.onResume();
  assert(hidden.reads==2 && hidden.reloadRequested &&
         hidden.uiRefreshes==beforeResume+1 &&
         "resuming during preparation must show its latest progress without a reload");
  hidden.preparationTask.pending.completion=1;
  hidden.update(0);
  assert(hidden.reads==3 && !hidden.reloadRequested);

  hidden.onPause();
  hidden.context.profileManager.profile.id="next";
  hidden.update(0);
  assert(hidden.displayedProfileId=="next" && !hidden.candidatesLoaded &&
         hidden.reads==3 && "hidden profile changes must invalidate stale candidates");
  hidden.onResume();
  assert(hidden.reads==4 && hidden.candidatesLoaded);
}
'''
        for key, code in (
            ("MUSIC_RESUME", extract(music, "void MusicPlayerScene::onResume()")),
            ("IR_RESUME", extract(ir, "void IrUploadsScene::onResume()")),
            ("IR_PROFILE", extract(ir, "void IrUploadsScene::refreshProfileIfNeeded()")),
            ("IR_OBSERVE", extract(ir, "void IrUploadsScene::observeRemoteRevisions()")),
            ("IR_PAUSE", extract(ir, "void IrUploadsScene::onPause()")),
            ("IR_UPDATE", extract(ir, "void IrUploadsScene::update(float)")),
            ("IR_PREPARATION", extract(ir, "void IrUploadsScene::applyPreparationUpdates()")),
        ):
            source = source.replace(key, code)
        self.compile_and_run(source)


if __name__ == "__main__":
    unittest.main()
