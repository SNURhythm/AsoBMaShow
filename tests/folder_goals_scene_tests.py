"""Exercise folder-goal editing and profile guards using production methods."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler

class FolderGoalsSceneTests(unittest.TestCase):
    def test_target_selection_validation_and_profile_switch(self):
        root=Path(__file__).resolve().parents[1]
        source=(root/'src/scene/FolderGoalsScene.cpp').read_text()
        methods='\n'.join(extract(source,s) for s in (
            'bool FolderGoalsScene::checkProfile(', 'void FolderGoalsScene::choose(',
            'void FolderGoalsScene::saveGoal(', 'void FolderGoalsScene::removeGoal(',
            'void FolderGoalsScene::openSource(', 'void FolderGoalsScene::addGoal(',
            'void FolderGoalsScene::changeScope(', 'bool FolderGoalsScene::occupied(',
            'void FolderGoalsScene::cancelEditor(', 'void FolderGoalsScene::back(',
            'void FolderGoalsScene::rememberScroll('))
        fixture=r'''
#include "goals/FolderGoal.h"
#include "goals/ChartGoal.h"
#include "goals/GoalNavigation.h"
#include <memory>
#include <functional>
#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <vector>
namespace long_note_mode { int valueFromId(const std::string &){return 1;} }
namespace i18n { std::string tr(const char *s){return s;} }
struct ScrollView {float offset=0;float getScrollOffset(){return offset;}};
int returned=0;bool returnToScene(int &,int){++returned;return true;}
struct FolderGoalsScene {
 struct {
  int manager=0; int *sceneManager=&manager;
  struct { std::string selectedLnMode="LN"; } settings;
  struct { struct Profile {std::string id="one";} p; Profile &activeProfile(){return p;} } profileManager;
  struct {
   int saves=0,removes=0; bool fail=false; folder_goals::Goal saved;
   std::optional<folder_goals::GoalProgress> PreviewFolderGoal(const folder_goals::Goal &g,std::string &error) {
    if(fail){error="unavailable";return {};}
    return folder_goals::GoalProgress{g,{.available=4}};
   }
   bool SaveFolderGoal(const folder_goals::Goal &g,std::string &error){if(fail){error="failed";return false;}++saves;saved=g;return true;}
   bool RemoveFolderGoal(const folder_goals::Goal &,std::string &){++removes;return true;}
  } scoreRepository;
 } context;
 std::shared_ptr<goal_navigation::State> navigationState_=std::make_shared<goal_navigation::State>();
 std::optional<folder_goals::Source> initial_;
 std::vector<folder_goals::GoalProgress> goals_;
 folder_goals::Goal draft_;
 std::optional<folder_goals::Progress> progress_;
 std::string profile_="one",message_,rate_="100";
 bool detail_=false,editing_=false,confirmRemove_=false,failed_=false,achieved_=false;
 bool picker_=false,newGoal_=false,sourceOrigin_=false;
 std::optional<folder_goals::Goal> editorReturn_;
 std::string renderedPage_;
 ScrollView *scroll_=nullptr,*navigationScroll_=nullptr;
 int returnTarget_=0;
 int offset_=0,reloads=0;
 void reload(){++reloads;}
 bool checkProfile();
 void choose(folder_goals::Source,int,folder_goals::Type);
 void saveGoal();void removeGoal();
 void later(std::function<void()> f){f();}
 void defer(std::function<bool()> f,int,bool){f();}
 void back();void rememberScroll();
 void openSource();void addGoal();void changeScope(int,folder_goals::Type);
 bool occupied() const;void cancelEditor();
};
METHODS
int main(){
 FolderGoalsScene s;
 folder_goals::Source source{.kind=folder_goals::SourceKind::Level,.identity="url",.level="1",.name="Level 1"};
 s.choose(source,2,folder_goals::Type::Lamp);
 assert(s.editing_ && s.detail_ && s.draft_.longNoteMode==2 && s.progress_->available==4);
 s.rate_="75.01";s.saveGoal();assert(s.context.scoreRepository.saved.target.percent==7501 && !s.editing_);
 s.editing_=true;s.rate_="0";s.saveGoal();assert(s.context.scoreRepository.saves==1 && s.editing_);
 s.rate_="80.001";s.saveGoal();assert(s.context.scoreRepository.saves==1);
 auto saved=s.context.scoreRepository.saved;s.goals_.push_back({saved,{}});
 s.choose(source,2,folder_goals::Type::Lamp);assert(!s.editing_ && s.rate_=="75.01");
 s.choose(source,2,folder_goals::Type::AverageEx);assert(s.editing_ && s.draft_.target.type==folder_goals::Type::AverageEx);
 // A source flag routes across all identities, including a sole Average EX goal.
 s.goals_.clear(); saved.target.type=folder_goals::Type::AverageEx;
 s.goals_.push_back({saved,{}});s.initial_=source;s.sourceOrigin_=true;s.openSource();
 assert(s.detail_ && !s.picker_ && !s.editing_ && s.draft_.target.type==folder_goals::Type::AverageEx);
 auto lamp=saved;lamp.target.type=folder_goals::Type::Lamp;s.goals_.push_back({lamp,{}});
 s.openSource();assert(s.picker_ && !s.detail_);
 s.addGoal();s.rate_="68.25";s.draft_.target.percent=6825;s.draft_.target.lamp=400;
 s.changeScope(2,folder_goals::Type::AverageEx);
 assert(s.rate_=="68.25" && s.draft_.target.percent==6825 && s.draft_.target.lamp==400);
 assert(s.occupied());auto count=s.context.scoreRepository.saves;s.saveGoal();
 assert(s.context.scoreRepository.saves==count && s.editing_);
 s.changeScope(3,folder_goals::Type::Lamp);assert(!s.occupied());
 s.cancelEditor();assert(!s.detail_ && !s.editing_ && s.picker_);
 s.choose(source,2,folder_goals::Type::AverageEx);s.editing_=true;
 s.changeScope(3,folder_goals::Type::Lamp);assert(s.draft_.longNoteMode==2 && s.draft_.target.type==folder_goals::Type::AverageEx);
 s.draft_.target.percent=1234;s.cancelEditor();
 assert(!s.editing_ && s.draft_.target.percent==saved.target.percent);
 s.addGoal();s.cancelEditor();assert(s.detail_ && !s.editing_ && s.draft_.target.type==folder_goals::Type::AverageEx);
 // Detail reached through a picker returns there; direct source detail exits.
 s.picker_=true;s.back();assert(!s.detail_ && returned==0);
 s.choose(source,2,folder_goals::Type::AverageEx);s.picker_=false;s.back();assert(returned==1);
 s.sourceOrigin_=false;s.back();assert(!s.detail_ && returned==1);
 ScrollView list{125},nav{23};s.scroll_=&list;s.navigationScroll_=&nav;
 s.renderedPage_="active:20";s.offset_=20;s.rememberScroll();
 assert(s.navigationState_->folderOffset==20 && s.navigationState_->folderScrollOffsets["active:20"]==125);
 s.achieved_=true;s.offset_=40;s.renderedPage_="achieved:40";list.offset=70;s.rememberScroll();
 assert(s.navigationState_->folderOffset==20 && s.navigationState_->folderAchievedOffset==40);
 assert(s.navigationState_->folderNavigationOffset==23);
 s.goals_.clear();s.openSource();assert(s.editing_ && s.newGoal_ && !s.picker_);
 s.sourceOrigin_=true;s.back();assert(returned==2);
 s.context.profileManager.p.id="two";assert(!s.checkProfile());
 assert(!s.detail_ && !s.editing_ && s.goals_.empty() && !s.progress_ && s.rate_=="100");
}
'''.replace('METHODS',methods)
        with tempfile.TemporaryDirectory(prefix='folder-goals-scene-') as directory:
            path=Path(directory)/'scene.cpp'; binary=Path(directory)/'scene';path.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [path,root/'src/goals/FolderGoal.cpp',root/'src/goals/ChartGoal.cpp'],binary,path.parent,includes=(root/'src',))
            subprocess.run([str(binary)],check=True,timeout=5)

if __name__=='__main__':unittest.main()
