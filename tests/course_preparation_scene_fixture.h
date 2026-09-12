#pragma once

#include "replay/CourseResultPersistence.h"

void testCourseEntryFactStageCountLimits() {
  AbortTemporaryDirectory temporary;
  const auto path = temporary.path / "course-stage-limit.bms";
  {
    std::ofstream file(path);
    file << "#PLAYER 1\n#TITLE Course stage limit\n#BPM 120\n#00011:01\n";
  }
  for (const std::size_t stageCount : {256U, 0U, 257U}) {
    auto session = std::make_shared<CoursePlaySession>();
    session->entries.resize(stageCount);
    for (auto &entry : session->entries) {
      entry.meta.BmsPath = path;
    }
    std::atomic_bool cancelled = false;
    std::string diagnostic;
    const auto revision = archive_file::debugLogRevision();
    const auto facts =
        play_options::prepareCourseEntryFacts(*session, cancelled, diagnostic);
    if (stageCount == 256U) {
      require(facts && facts->size() == 256U &&
                  std::all_of(facts->begin(), facts->end(), [](const auto &entry) {
                    return entry.totalNotes == 1;
                  }),
              "PR105 all 256 unplayed course entries prepare without truncation");
      require(archive_file::debugLogRevision() > revision,
              "PR105 the boundary fixture exercises real chart parsing");
      continue;
    }
    require(archive_file::debugLogRevision() == revision,
            "PR105 invalid course stage counts reject before any chart parsing");
    require(!facts && !diagnostic.empty(),
            "PR105 empty and 257-stage courses return no facts with a diagnostic");
    require(session->entries.size() == stageCount,
            "PR105 invalid courses are rejected rather than truncated");

    session->modernCourseAttemptId = "123e4567-e89b-42d3-a456-426614174000";
    ResultScene scene;
    scene.local.courseOptions.session = session;
    scene.context.persistModernCourse = [](const auto &) {
      require(false, "PR105 invalid course preparation must not reach persistence");
      return replay::CourseResultPersistenceOutcome{};
    };
    require(scene.persistModernCourseResult(),
            "PR105 ResultScene owns invalid courses instead of falling back to legacy");
    require(!session->modernCourseAttempt &&
                session->modernCoursePersistenceOutcome &&
                session->modernCoursePersistenceOutcome->state ==
                    replay::CourseResultPersistenceState::InvalidAttempt &&
                scene.local.persistenceOptions.outcome.state ==
                    result_persistence::SaveState::InvalidAttempt &&
                session->modernCourseDiagnostic == diagnostic,
            "PR105 preparation failure remains an InvalidAttempt in result presentation");
    require(archive_file::debugLogRevision() == revision,
            "PR105 ResultScene rejects invalid course counts without parsing charts");
  }
}

void testCourseEntryFactPreparationBoundaries() {
  testCourseEntryFactStageCountLimits();
  CoursePlaySession session;
  session.longNoteMode = 2;
  session.entries.resize(1);
  session.entries[0].meta.BmsPath = "unavailable-course-fact-fixture.bms";
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.KeyMode = 7;
  chart->Meta.TotalNotes = 99;
  auto *measure = new bms_parser::Measure;
  auto *headTime = new bms_parser::TimeLine(8, false);
  auto *tailTime = new bms_parser::TimeLine(8, false);
  headTime->Timing = 1'000'000;
  tailTime->Timing = 2'000'000;
  int lane = 0;
  for (const auto type : {bms_parser::LongNoteType::Undefined,
                          bms_parser::LongNoteType::LongNote,
                          bms_parser::LongNoteType::ChargeNote,
                          bms_parser::LongNoteType::HellChargeNote}) {
    auto *head = new bms_parser::LongNote(1, type);
    auto *tail = new bms_parser::LongNote(1, type);
    head->Tail = tail;
    tail->Head = head;
    headTime->SetNote(lane, head);
    tailTime->SetNote(lane, tail);
    ++lane;
  }
  headTime->SetNote(7, new bms_parser::Note(1));
  measure->TimeLines = {headTime, tailTime};
  chart->Measures.push_back(measure);
  session.preparedCourseCharts.push_back(std::move(chart));
  std::atomic_bool cancelled = false;
  std::string diagnostic;
  auto facts = play_options::prepareCourseEntryFacts(session, cancelled, diagnostic);
  require(facts && facts->front().totalNotes == 8,
          "COR04 prepared mixed LN types are counted without guessed metadata conversion or reparse");
  require(session.preparedCourseCharts[0]->Meta.TotalNotes == 99 &&
              session.preparedCourseCharts[0]->Meta.LnMode == 0,
          "COR04 facts do not mutate a retained prepared chart");
  session.longNoteMode = 1;
  facts = play_options::prepareCourseEntryFacts(session, cancelled, diagnostic);
  require(facts && facts->front().totalNotes == 7,
          "COR04 selected fallback affects only undefined LN tails");
  cancelled = true;
  require(!play_options::prepareCourseEntryFacts(session, cancelled, diagnostic),
          "COR04 cancellation cannot publish incomplete course facts");
  cancelled = false;
  session.preparedCourseCharts.clear();
  require(!play_options::prepareCourseEntryFacts(session, cancelled, diagnostic),
          "COR04 missing unplayed charts fail explicitly instead of guessing maximum score");
}

void testEffectiveCourseFactsPersistThroughResultScene() {
  testCourseEntryFactPreparationBoundaries();
  for (const int selectedMode : {2, 3}) {
    for (const int playedStages : {1, 2}) {
      AbortTemporaryDirectory temporary;
      auto session = std::make_shared<CoursePlaySession>();
      session->courseName = "Effective LN course";
      session->longNoteMode = selectedMode;
      session->gaugeType = GaugeType::Hard;
      session->modernCourseAttemptId = "123e4567-e89b-42d3-a456-426614174000";
      session->modernCoursePlayedAtUnixMillis = 1'700'000'000'000;
      std::vector<std::unique_ptr<bms_parser::Chart>> charts;
      for (int stageIndex = 0; stageIndex < 2; ++stageIndex) {
        const auto path = temporary.path / ("stage-" + std::to_string(stageIndex) + ".bms");
        {
          std::ofstream file(path);
          file << "#PLAYER 1\n#TITLE Effective course\n#BPM 120\n#TOTAL 120\n"
                  "#LNOBJ ZZ\n#00011:0100ZZ00\n#00012:00010000\n";
          if (stageIndex == 1) {
            file << "#RANDOM 2\n#IF 1\n#00013:00000000\n#ENDIF\n"
                    "#IF 2\n#00013:00000100\n#ENDIF\n#ENDRANDOM\n";
          }
        }
        std::atomic_bool cancelled = false;
        auto chart = play_options::parseChart(
            path, 42U, std::string(bms_parser::Parser::RandomPrngId),
            stageIndex == 1 ? std::optional<std::vector<int>>({2}) : std::nullopt,
            cancelled, "course fact fixture");
        require(chart && !cancelled, "COR04 tiny course fixture parses");
        require(chart->Meta.TotalNotes == stageIndex + 2,
                "COR04 library facts count the undefined LN head only");
        session->entries.push_back({.meta = chart->Meta});
        applyEffectiveLongNoteModeToChart(*chart, selectedMode);
        require(chart->Meta.TotalNotes == stageIndex + 3,
                "COR04 prepared CN/HCN includes the authored tail");
        charts.push_back(std::move(chart));
      }
      const auto libraryFirst = session->entries[0].meta;
      const auto librarySecond = session->entries[1].meta;
      session->courseKey = course_identity::makeCourseKey(*session);
      const std::string courseKey = session->courseKey;
      for (int stageIndex = 0; stageIndex < playedStages; ++stageIndex) {
        auto &chart = *charts[stageIndex];
        StartOptions options;
        options.longNoteMode = selectedMode;
        options.gaugeType = GaugeType::Hard;
        const auto policy = buildGameplayRulesetPolicyAtPlayStart(
            options, chart.Meta, AppSettings::NotePriorityMode::Lowest);
        require(policy.built(), policy.diagnostic);
        const auto provenance = captureScoreProvenanceAtPlayStart(options, chart.Meta, *policy.policy);
        RhythmState state(&chart, false, policy.policy->gauge);
        state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
        for (int noteIndex = 0; noteIndex < chart.Meta.TotalNotes; ++noteIndex) {
          state.commitJudge(JudgeResult(PGreat, 0));
        }
        std::string diagnostic;
        const auto captured = result_persistence::captureModernCourseStageResult(
            stageIndex, chart.Meta, state, provenance, selectedMode, diagnostic);
        require(captured.has_value(), diagnostic);
        session->currentIndex = stageIndex;
        session->recordResult(chart.Meta, state);
        require(session->recordModernCourseStage(*captured, {}),
                "COR04 completed stage facts enter the real session");
        std::filesystem::remove(chart.Meta.BmsPath);
      }
      ResultScene scene;
      scene.local.courseOptions.session = session;
      scene.local.resultState = session->completedResults.back().state;
      scene.local.attemptProvenance = session->completedResults.empty()
                                            ? ScoreProvenance::Legacy()
                                            : session->modernCourseStageResults.back().score.provenance;
      const auto replayPath = temporary.path / "replay.db";
      const auto scorePath = temporary.path / "score.db";
      {
        ReplayRepository repository(replayPath);
        ScoreRepository scores(scorePath);
        require(repository.EnsureSchema() && scores.EnsureSchema(),
                "COR04 real temporary repositories initialize");
        replay::CourseResultPersistence persistence(scores, repository);
        scene.context.persistModernCourse = [&](const auto &attempt) {
          return persistence.persist(attempt);
        };
        require(scene.persistModernCourseResult(), "COR04 actual ResultScene owns persistence");
        require(session->modernCourseAttempt.has_value(), session->modernCourseDiagnostic);
        require(session->modernCoursePersistenceOutcome && session->modernCoursePersistenceOutcome->saved(),
                session->modernCourseDiagnostic);
      }
      ReplayRepository reopened(replayPath);
      const auto loaded = reopened.LoadModernCourseResultByAttempt(session->modernCourseAttemptId);
      require(loaded.record.has_value(), "COR04 persisted modern course reopens");
      const auto &result = loaded.record->result;
      require(result.maxScore == 14 && result.completedCharts == playedStages &&
                  result.entryFacts[0].totalNotes == 3 && result.entryFacts[1].totalNotes == 4,
              "COR04 complete and partial courses preserve played/unplayed effective maximum scores");
      require(result.stages[0].score.maxScore == 6 &&
                  (playedStages == 1 || result.stages[1].score.maxScore == 8),
              "COR04 played maximum scores survive durable capture unchanged");
      require(session->entries[0].meta.TotalNotes == libraryFirst.TotalNotes &&
                  session->entries[1].meta.TotalNotes == librarySecond.TotalNotes &&
                  session->entries[1].meta.RandomSeed == librarySecond.RandomSeed &&
                  session->entries[1].meta.RandomValues == librarySecond.RandomValues &&
                  session->courseKey == courseKey,
              "COR04 fact preparation preserves library metadata, random branch, and course identity");
    }
  }
  std::cout << "COR04 actual result scene persistence tests passed\n";
}
