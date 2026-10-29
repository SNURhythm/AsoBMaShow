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
            'void FolderGoalsScene::saveGoal(', 'void FolderGoalsScene::removeGoal('))
        fixture=r'''
#include "goals/FolderGoal.h"
#include "goals/ChartGoal.h"
#include "goals/GoalNavigation.h"
#include <memory>
#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <vector>
namespace i18n { std::string tr(const char *s){return s;} }
struct FolderGoalsScene {
 struct {
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
 int offset_=0,reloads=0;
 void reload(){++reloads;}
 bool checkProfile();
 void choose(folder_goals::Source,int,folder_goals::Type);
 void saveGoal();void removeGoal();
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
