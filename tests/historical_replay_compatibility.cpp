// This harness is compiled against the archived application for capture and the
// current application for checks. Never regenerate historical evidence with the
// current parser. See fixtures/historical_replays/README.md.
#include "ReplayData.h"
#include "FileChecksum.h"
#include "ReplayLegacyFixture.h"
#ifndef HISTORICAL_REPLAY_CAPTURE
#include "ChartPlayability.h"
#endif
#include "ScoreProvenance.h"
#include "bms_parser.hpp"
#include "replay/ReplayPlaybackMaterializer.h"
#include "replay/ReplayCapabilities.h"
#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayRulesetPolicy.h"
#include "scene/play/Pacemaker.h"
#include "scene/play/ReplayKeysoundSchedule.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using Json = nlohmann::ordered_json;
using namespace replay;
using result_persistence::ModernChartResult;

namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
Json readJson(const std::filesystem::path &path) {
  std::ifstream stream(path);
  require(bool(stream), "Cannot read " + path.string());
  return Json::parse(stream);
}
void writeJson(const std::filesystem::path &path, const Json &value) {
  std::ofstream stream(path);
  stream << value.dump(2) << '\n';
  require(bool(stream), "Cannot write " + path.string());
}
Json resultJson(const ModernChartResult &r) {
  const auto &s = r.score;
  Json timing = Json::array();
  if (r.judgementTiming) for (auto v : r.judgementTiming->byJudgement)
    timing.push_back({v.fast, v.slow});
  return {{"attempt", r.attemptId}, {"path", s.chartPath},
          {"md5", s.chartMd5}, {"sha256", s.chartSha256},
          {"title", s.chartTitle}, {"artist", s.chartArtist},
          {"lnMode", s.longNoteMode}, {"keyMode", r.keyMode},
          {"facts", {s.score,s.maxScore,s.maxCombo,s.comboBreak,s.pGreat,s.great,
                     s.good,s.bad,s.poor,s.kPoor,s.fast,s.slow,s.clearType}},
          {"gauge", s.finalGauge}, {"gaugeType", int(r.adoptedGaugeType)},
          {"gaugeHistory", r.adoptedGaugeHistory}, {"timing", timing},
          {"provenance", serializeScoreProvenance(s.provenance)},
          {"playedAt", r.playedAtUnixMillis}, {"fingerprint", r.resultFingerprint}};
}
ModernChartResult resultFromJson(const Json &j) {
  ModernChartResult r;
  auto &s = r.score;
  r.attemptId=j.at("attempt"); s.chartPath=j.at("path");
  s.chartMd5=j.at("md5"); s.chartSha256=j.at("sha256");
  s.chartTitle=j.at("title"); s.chartArtist=j.at("artist");
  s.longNoteMode=j.at("lnMode"); r.keyMode=j.at("keyMode");
  const auto &f=j.at("facts");
  int *fields[]={&s.score,&s.maxScore,&s.maxCombo,&s.comboBreak,&s.pGreat,&s.great,
                &s.good,&s.bad,&s.poor,&s.kPoor,&s.fast,&s.slow,&s.clearType};
  for (std::size_t i=0;i<std::size(fields);++i) *fields[i]=f.at(i);
  s.finalGauge=j.at("gauge"); r.adoptedGaugeType=GaugeType(j.at("gaugeType").get<int>());
  r.adoptedGaugeHistory=j.at("gaugeHistory").get<std::vector<float>>();
  if (!j.at("timing").empty()) {
    r.judgementTiming.emplace();
    for (std::size_t i=0;i<JudgementCount;++i)
      r.judgementTiming->byJudgement[i]={j["timing"][i][0],j["timing"][i][1]};
  }
  std::string error;
  auto provenance=deserializeScoreProvenance(j.at("provenance").get<std::string>(), error);
  require(provenance.has_value(), error); s.provenance=*provenance;
  r.playedAtUnixMillis=j.at("playedAt"); r.resultFingerprint=j.at("fingerprint");
  require(result_persistence::modernResultFingerprint(r)==r.resultFingerprint,
          "Historical result fingerprint was modified");
  return r;
}
Json eventsJson(const ReplayData &r) {
  Json j=Json::array();
  for (const auto &e:r.events) j.push_back({int(e.action),e.lane,e.noteTimeMicros,
      e.songTimeMicros,e.judgeTimeMicros,int(e.judgement),e.diffMicros,e.gauge,
      int(e.gaugeType),e.combo,e.score});
  return j;
}
Json observations(bms_parser::Chart &chart, const ReplayData &r) {
  const auto lookup=pacemaker::buildReplayNoteLookup(chart);
  int addressed=0,matched=0;
  for (const auto &e:r.events) if (e.noteTimeMicros>=0) {
    ++addressed; matched+=pacemaker::findReplayNote(lookup,e)!=nullptr;
  }
  Json notes=Json::array(),sounds=Json::array();
  const auto definition=gameplay::buildGameplayDefinition(chart,1);
  for (auto id:definition.chronologicalNotes()) {
    const auto &n=definition.note(id);
    notes.push_back({n.lane,n.timingMicros,n.wav,int(n.kind),n.pairId});
  }
  for (const auto &s:buildReplayKeysoundSchedule(definition,r.events,0,std::nullopt))
    sounds.push_back({s.timeMicros,s.wav});
  return {{"keyMode",chart.Meta.KeyMode},{"totalNotes",chart.Meta.TotalNotes},
          {"notes",notes},{"lookupAddressed",addressed},{"lookupMatched",matched},
          {"ghost",pacemaker::buildReplayScoreProgression(chart,r)},{"keysounds",sounds}};
}
std::unique_ptr<bms_parser::Chart> parse(const std::filesystem::path &path,
                                       bool buffered, int randomValue) {
  bms_parser::Parser parser; parser.SetRandomSeed(123456);
  parser.SetRandomValues({randomValue});
  std::atomic_bool cancelled=false; bms_parser::Chart *raw=nullptr;
  if (buffered) {
    std::ifstream stream(path,std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),{});
#ifdef HISTORICAL_REPLAY_CAPTURE
    parser.Parse(bytes,&raw,false,false,cancelled);
#else
    parser.Parse(bytes,&raw,false,false,cancelled,path);
#endif
  } else
  parser.Parse(path,&raw,false,false,cancelled);
  require(raw!=nullptr,"Chart parse failed");
  raw->Meta.BmsPath=path;
  return std::unique_ptr<bms_parser::Chart>(raw);
}
ModernChartResult initialResult(const bms_parser::Chart &chart) {
  ModernChartResult r; r.attemptId="123e4567-e89b-42d3-a456-426614174000";
  r.playedAtUnixMillis=1700000000123LL;
  auto &s=r.score; s.chartPath=chart.Meta.BmsPath.filename().string();
  s.chartMd5=chart.Meta.MD5; s.chartSha256=chart.Meta.SHA256;
  s.chartTitle=chart.Meta.Title; s.chartArtist=chart.Meta.Artist;
  s.longNoteMode=1; s.maxScore=chart.Meta.TotalNotes*2; r.keyMode=chart.Meta.KeyMode;
  auto policy=gameplay::buildGameplayRulesetPolicy(chart.Meta,{});
  require(policy.built(),policy.diagnostic);
  ScoreProvenanceBuildInput p; p.chartMeta=chart.Meta; p.longNoteMode=1;
  p.sourceJudgeRank=chart.Meta.Rank; p.totalNotes=chart.Meta.TotalNotes;
  p.effectiveGaugeTotal=policy.policy->gauge.effectiveTotal;
  p.authoredGaugeTotal=chart.Meta.Total;
  p.effectiveJudgeContexts=policy.policy->judge.rules().contexts;
  p.inputDevices={InputDeviceCategory::Keyboard};
  s.provenance=makeScoreProvenance(p);
  return r;
}
ReplayChartDocument document(const ModernChartResult &saved) {
  ReplayChartDocument d; auto &s=d.playback.setup; const auto &p=saved.score.provenance;
  s.chart={.md5=saved.score.chartMd5,.sha256=saved.score.chartSha256,.keyMode=saved.keyMode};
  s.longNoteMode=1; s.ruleset=p.ruleset; s.gaugeProfile=p.gaugeProfile;
  s.initialGaugeType=p.gaugeType; s.playback=p.playback;
  s.candidateSelection=p.stages.front().candidateSelection;
  return d;
}
void fillInput(ReplayChartDocument &d, const bms_parser::Chart &chart) {
  const auto def=gameplay::buildGameplayDefinition(chart,1);
  for (auto id:def.chronologicalNotes()) {
    const auto &n=def.note(id);
    if (n.kind==gameplay::NoteKind::LongTail || n.kind==gameplay::NoteKind::Landmine) continue;
    auto control=logicalControlForChartLane(chart.Meta.KeyMode,n.lane,n.scratchLane);
    require(control.has_value(),"Cannot map capture lane");
    d.playback.input.push_back({.songTimeMicros=n.timingMicros,.control=*control,.pressed=true});
    auto release=n.timingMicros+10000;
    if (n.kind==gameplay::NoteKind::LongHead && n.pairId!=gameplay::kInvalidNoteId)
      release=def.note(n.pairId).timingMicros;
    d.playback.input.push_back({.songTimeMicros=release,.control=*control,.pressed=false});
  }
  std::stable_sort(d.playback.input.begin(),d.playback.input.end(),[](auto &a,auto &b){return a.songTimeMicros<b.songTimeMicros;});
  d.timeBounds.completionSongTimeMicros=def.metadata().finalTimelineTimeMicros+2000000;
}
Json inputJson(const ReplayChartDocument &d) {
  Json j=Json::array();
  for (const auto &i:d.playback.input) j.push_back({i.songTimeMicros,int(i.control.kind),i.control.player,i.control.lane,i.pressed});
  return j;
}
void loadInput(ReplayChartDocument &d,const Json &j) {
  for (const auto &i:j) d.playback.input.push_back({.songTimeMicros=i[0],
      .control={.kind=LogicalControlKind(i[1].get<int>()),.player=i[2],.lane=i[3]},.pressed=i[4]});
}
std::string sqlText(const std::string &value) {
  char *quoted=sqlite3_mprintf("%Q",value.c_str());
  std::string result=quoted; sqlite3_free(quoted); return result;
}
void saveLegacy(sqlite3 *db,int id,const ModernChartResult &saved,const ReplayData &r) {
  const auto &s=saved.score;
  std::ostringstream sql; sql << std::setprecision(std::numeric_limits<float>::max_digits10);
  sql << "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
      "ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,final_gauge,clear_type,"
      "random_seed,random_prng,random_values,play_option,assist_option,created_at) VALUES("
      << id << ',' << sqlText(s.chartPath) << ',' << sqlText(s.chartMd5) << ','
      << sqlText(s.chartSha256) << ',' << sqlText(s.chartTitle) << ',' << sqlText(s.chartArtist)
      << ",1," << int(r.initialGaugeType) << ",0," << s.score << ',' << s.maxCombo << ','
      << s.finalGauge << ',' << s.clearType << ",123456,'std::mt19937_64',"
      << sqlText(Json(r.randomValues).dump()) << ",'NORMAL','OFF','2023-11-14 22:13:20');";
  int index=0;
  for (const auto &e:r.events) sql << "INSERT INTO replay_events(replay_id,event_index,action,"
      "lane,note_time_micros,song_time_micros,judge_time_micros,judgement,diff_micros,"
      "gauge,gauge_type,combo,score) VALUES(" << id << ',' << index++ << ',' << int(e.action)
      << ',' << e.lane << ',' << e.noteTimeMicros << ',' << e.songTimeMicros << ','
      << e.judgeTimeMicros << ',' << int(e.judgement) << ',' << e.diffMicros << ','
      << e.gauge << ',' << int(e.gaugeType) << ',' << e.combo << ',' << e.score << ");";
  std::string error;
  require(replay_legacy_fixture::execute(db,sql.str(),error),error);
}
ReplayData loadLegacy(sqlite3 *db,int id,const ModernChartResult &saved) {
  sqlite3_stmt *stmt=nullptr;
  require(sqlite3_prepare_v2(db,"SELECT action,lane,note_time_micros,song_time_micros,"
      "judge_time_micros,judgement,diff_micros,gauge,gauge_type,combo,score "
      "FROM replay_events WHERE replay_id=? ORDER BY event_index",-1,&stmt,nullptr)==SQLITE_OK,
      "Cannot read historical SQLite events");
  sqlite3_bind_int(stmt,1,id);
  ReplayData r; r.provenance=saved.score.provenance;
  while (sqlite3_step(stmt)==SQLITE_ROW) r.events.push_back({
      .action=ReplayEventAction(sqlite3_column_int(stmt,0)),.lane=sqlite3_column_int(stmt,1),
      .noteTimeMicros=sqlite3_column_int64(stmt,2),.songTimeMicros=sqlite3_column_int64(stmt,3),
      .judgeTimeMicros=sqlite3_column_int64(stmt,4),.judgement=Judgement(sqlite3_column_int(stmt,5)),
      .diffMicros=sqlite3_column_int64(stmt,6),.gauge=float(sqlite3_column_double(stmt,7)),
      .gaugeType=GaugeType(sqlite3_column_int(stmt,8)),.combo=sqlite3_column_int(stmt,9),
      .score=sqlite3_column_int(stmt,10)});
  sqlite3_finalize(stmt); return r;
}
void saveBrd(const std::filesystem::path &path,const ReplayChartDocument &d,
             const ModernChartResult &saved) {
  std::string error;
  auto bytes=BeatorajaReplayCodec{}.encodeChart(d,saved.playedAtUnixMillis,error);
  require(bytes.has_value(),"Historical BRD capture failed: "+error);
  std::ofstream out(path,std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes->data()),bytes->size());
  require(bool(out),"Cannot save historical BRD");
}
ReplayChartDocument loadBrd(const std::filesystem::path &path,
                            const ReplayChartDocument &expected) {
  std::ifstream in(path,std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});
  auto decoded=BeatorajaReplayCodec{}.decode(
      std::as_bytes(std::span(bytes)),
      {.stageKeyModes={expected.playback.setup.chart.keyMode},.stageTimeBounds={expected.timeBounds}});
  require(decoded.chart.has_value(),"Historical BRD decode failed: "+decoded.diagnostic);
  require(*decoded.chart==expected,"Historical BRD differs from captured raw input/setup");
  return *decoded.chart;
}
struct Case { const char *name; const char *chart; bool buffered=false; int randomValue=1; };
const Case cases[]={
  {"ordinary","ordinary.bms"},{"fractional_stop","fractional_stop.bms"},
  {"long_notes","long_notes.bms"},{"lnobj","lnobj.bms"},
  {"random_1","random.bms",false,1},{"random_2","random.bms",false,2},
  {"pms_path","mapping.pms"},{"pms_buffered","mapping.pms",true},
  {"orphan","orphan.bms"},{"detached_head","detached_head.bms"},
  {"detached_tail","detached_tail.bms"}};

void run(const std::filesystem::path &root, bool capture) {
  Json report=Json::array();
  if (!capture) {
    const auto manifest=readJson(root/"manifest.json");
    for (auto it=manifest["files"].begin();it!=manifest["files"].end();++it) {
      std::string error;
      const auto hash=file_checksum::sha256File(root/it.key(),error);
      require(hash && *hash==it.value().get<std::string>(),
              "Immutable historical evidence changed: "+it.key());
    }
  }
  const auto database=root/"legacy.sqlite";
  std::string databaseError;
  if (capture) require(replay_legacy_fixture::replaceWithVersion2Database(
      database,13,{},databaseError),databaseError);
  sqlite3 *rawDb=nullptr;
  require(sqlite3_open_v2(database.string().c_str(),&rawDb,
      capture ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,
      "Cannot open historical legacy database");
  std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(rawDb,sqlite3_close);
  int caseId=0;
  ReplayPlaybackCarryState newCarry;
  ReplayCourseDocument course;
  Json courseEvidence=Json::array();
  int courseScore=0;
  int courseClearType=kClearTypeFailedRank;
  for (const auto &c:cases) {
    ++caseId;
    auto chart=parse(root/"charts"/c.chart,c.buffered,c.randomValue);
    const auto path=root/(std::string(c.name)+".json");
    if (capture) {
      auto saved=initialResult(*chart); auto d=document(saved); fillInput(d,*chart);
      d.playback.setup.chartRandomValues=chart->Meta.RandomValues;
      d.playback.setup.chartRandomSeed=chart->Meta.RandomSeed;
      d.playback.setup.chartRandomPrng=chart->Meta.RandomPrng;
      auto outcome=ReplayPlaybackMaterializer::materializeForConsumers(d,saved,*chart);
      require(outcome.judgedResult && outcome.replayData,"Capture judging failed: "+outcome.diagnostic);
      saved=*outcome.judgedResult;
      std::string savedDiagnostic;
      require(result_persistence::validateModernChartResult(saved,savedDiagnostic),
              "Captured saved result invalid: "+savedDiagnostic);
      const auto verified=ReplayPlaybackMaterializer::materializeForConsumers(d,saved,*chart);
      require(verified.matched(),"Historical result does not reproduce");
      saveBrd(root/(std::string(c.name)+".brd"),d,saved);
      saveLegacy(db.get(),caseId,saved,*verified.replayData);
      writeJson(path,{{"chart",c.chart},{"buffered",c.buffered},
        {"randomValues",chart->Meta.RandomValues},
        {"randomSeed",chart->Meta.RandomSeed.value_or(0)},
        {"randomPrng",chart->Meta.RandomPrng.value_or("")},{"input",inputJson(d)},
        {"completion",d.timeBounds.completionSongTimeMicros},{"saved",resultJson(saved)},
        {"legacyEvents",eventsJson(*verified.replayData)},
        {"observations",observations(*chart,*verified.replayData)}});
    } else {
      const auto historical=readJson(path); const auto saved=resultFromJson(historical["saved"]);
      require(saved.score.chartSha256==chart->Meta.SHA256,"Chart bytes changed");
      auto d=document(saved); loadInput(d,historical["input"]);
      d.timeBounds.completionSongTimeMicros=historical["completion"];
      d.playback.setup.chartRandomValues=historical["randomValues"].get<std::vector<int>>();
      d.playback.setup.chartRandomSeed=historical["randomSeed"].get<unsigned int>();
      d.playback.setup.chartRandomPrng=historical["randomPrng"].get<std::string>();
      d=loadBrd(root/(std::string(c.name)+".brd"),d);
      auto legacy=loadLegacy(db.get(),caseId,saved);
      require(eventsJson(legacy)==historical["legacyEvents"],"Historical SQLite event evidence changed");
#ifndef HISTORICAL_REPLAY_CAPTURE
      if (const auto error=chart_playability::error(*chart)) {
        report.push_back({{"case",c.name},{"admitted",false},{"diagnostic",*error}});
        continue;
      }
#endif
      auto now=observations(*chart,legacy);
      auto outcome=ReplayPlaybackMaterializer::materializeForConsumers(d,saved,*chart);
      const bool identity=chart->Meta.KeyMode==saved.keyMode;
      Json row={{"case",c.name},{"admitted",true},{"identityMatch",identity},
        {"randomSetupMatch",chart->Meta.RandomValues==d.playback.setup.chartRandomValues &&
           chart->Meta.RandomSeed==d.playback.setup.chartRandomSeed &&
           chart->Meta.RandomPrng==d.playback.setup.chartRandomPrng},
        {"lookup",{now["lookupMatched"],now["lookupAddressed"]}},
        {"notesEqual",now["notes"]==historical["observations"]["notes"]},
        {"ghostEqual",now["ghost"]==historical["observations"]["ghost"]},
        {"keysoundsEqual",now["keysounds"]==historical["observations"]["keysounds"]},
        {"resultMatched",outcome.matched()},
        {"fingerprintEqual",outcome.judgedResult && outcome.judgedResult->resultFingerprint==saved.resultFingerprint},
        {"currentObservations",now},{"diagnostic",outcome.diagnostic}};
#ifndef HISTORICAL_REPLAY_CAPTURE
      row["judgedFingerprint"]=outcome.judgedResult ? outcome.judgedResult->resultFingerprint : "";
      row["judgedFacts"]=outcome.judgedResult ? resultJson(*outcome.judgedResult)["facts"] : Json();
      row["consumerIdentityCompatible"]=outcome.consumerIdentityCompatible;
      row["savedPlaybackReady"]=identity && outcome.matched() && outcome.consumerIdentityCompatible;
#endif
      report.push_back(row);
    }
  }
  // Course evidence uses the same two immutable stage inputs, but genuine carried
  // gauge/combo/max-combo from stage one when capturing and checking stage two.
  for (const auto *name:{"ordinary","fractional_stop"}) {
    auto j=readJson(root/(std::string(name)+".json"));
    auto chart=parse(root/"charts"/j["chart"].get<std::string>(),false,1);
    auto saved=capture ? initialResult(*chart) : resultFromJson(readJson(root/(std::string("course_")+name+".json")));
    auto d=document(saved); loadInput(d,j["input"]); d.timeBounds.completionSongTimeMicros=j["completion"];
    course.playback.stages.push_back(d.playback);
    course.timeBounds.push_back(d.timeBounds);
    course.playback.restMicrosAfterStage.push_back(std::string(name)=="ordinary" ? 1'000'000 : 0);
    auto o=ReplayPlaybackMaterializer::materializeForConsumers(d,saved,*chart,newCarry);
    require(o.judgedResult && o.finalGaugeState,"Course capture/check failed: "+o.diagnostic);
    if (capture) {
      writeJson(root/(std::string("course_")+name+".json"),resultJson(*o.judgedResult));
      saveLegacy(db.get(),++caseId,*o.judgedResult,*o.replayData);
      courseEvidence.push_back({{"legacyEvents",eventsJson(*o.replayData)},
                                {"observations",observations(*chart,*o.replayData)}});
    } else {
      const auto evidence=readJson(root/"course_stage_evidence.json")[course.playback.stages.size()-1];
      auto legacy=loadLegacy(db.get(),++caseId,saved);
      require(eventsJson(legacy)==evidence["legacyEvents"],"Historical course SQLite evidence changed");
      const auto observed=observations(*chart,legacy);
      report.push_back({{"case",std::string("course_")+name},{"resultMatched",o.matched()},
        {"fingerprintEqual",o.judgedResult->resultFingerprint==saved.resultFingerprint},
        {"endingCombo",o.endingCombo},{"diagnostic",o.diagnostic},
        {"lookup",{observed["lookupMatched"],observed["lookupAddressed"]}},
        {"ghostEqual",observed["ghost"]==evidence["observations"]["ghost"]},
        {"keysoundsEqual",observed["keysounds"]==evidence["observations"]["keysounds"]}});
    }
    courseScore+=o.judgedResult->score.score;
    courseClearType=o.judgedResult->score.clearType;
    newCarry={.gauge=o.finalGaugeState,.combo=o.endingCombo,.maximumCombo=o.judgedResult->score.maxCombo};
  }
  const auto coursePath=root/"course.brd";
  if (capture) {
    writeJson(root/"course_stage_evidence.json",courseEvidence);
    std::ostringstream sql;
    sql << std::setprecision(std::numeric_limits<float>::max_digits10);
    sql << "INSERT INTO course_replays(id,course_id,course_name,gauge_type,gauge_auto_shift,"
        "final_score,max_combo,final_gauge,clear_type,completed_charts,total_charts,created_at) "
        "VALUES(1,1,'Historical course'," << int(course.playback.stages.front().setup.initialGaugeType)
        << ',' << int(course.playback.stages.front().setup.gaugeAutoShift) << ','
        << courseScore << ',' << newCarry.maximumCombo
        << ',' << newCarry.gauge->currentGauge << ',' << courseClearType
        << ",2,2,'2023-11-14 22:13:20');"
        "INSERT INTO course_replay_stages(course_replay_id,stage_index,replay_id,rest_micros_after_stage) "
        "VALUES(1,0," << std::size(cases)+1 << ",1000000),(1,1," << std::size(cases)+2 << ",0);";
    require(replay_legacy_fixture::execute(db.get(),sql.str(),databaseError),databaseError);
    std::string diagnostic;
    auto bytes=BeatorajaReplayCodec{}.encodeCourse(course,1700000000123LL,diagnostic);
    require(bytes.has_value(),diagnostic);
    std::ofstream stream(coursePath,std::ios::binary);
    stream.write(reinterpret_cast<const char *>(bytes->data()),bytes->size());
    require(bool(stream),"Cannot save historical course BRD");
  } else {
    std::ifstream stream(coursePath,std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),{});
    auto decoded=BeatorajaReplayCodec{}.decode(std::as_bytes(std::span(bytes)),
        {.stageKeyModes={course.playback.stages[0].setup.chart.keyMode,
                         course.playback.stages[1].setup.chart.keyMode},
         .stageTimeBounds=course.timeBounds});
    require(decoded.course && *decoded.course==course,"Historical course BRD setup/input changed");
  }
  if (!capture) {
    for (const auto origin:{RecordOrigin::LegacyChartSummary,RecordOrigin::LegacyCourseSummary}) {
      const auto capabilities=capabilitiesFor({.origin=origin,.replayState=ReplayState::Verified});
      require(capabilities.recordsList && !capabilities.watch && !capabilities.retrySame &&
              !capabilities.gBattle && !capabilities.practiceGhost && !capabilities.videoExport,
              "Legacy replay must remain history-only without semantic equivalence evidence");
    }
    std::cout << report.dump(2) << '\n';
    auto expectedPath=root/"expected_comparison.json";
    require(readJson(expectedPath)==report,"Historical replay comparison changed; inspect reported evidence");
  }
}
}
int main(int argc,char **argv) {
  try {
#ifdef ASOBMASHOW_SOURCE_DIR
    if (argc==1) {
      run(std::filesystem::path(ASOBMASHOW_SOURCE_DIR)/"tests/fixtures/historical_replays",false);
      return 0;
    }
#endif
    require(argc>=2,"Usage: historical_replay_compatibility FIXTURE_DIR [--capture]");
    bool capture=argc==3 && std::string(argv[2])=="--capture";
#ifndef HISTORICAL_REPLAY_CAPTURE
    require(!capture,"Capture is only allowed in the archived-parser build");
#endif
    run(argv[1],capture); return 0;
  } catch(const std::exception &e) {std::cerr << e.what() << '\n'; return 1;}
}
