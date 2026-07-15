#include "../src/AppDatabaseInitializer.h"
#include "../src/ResultPersistenceCoordinator.h"

#include <iostream>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<MusicPlaylistRepository>);
static_assert(!std::is_copy_assignable_v<MusicPlaylistRepository>);
static_assert(!std::is_move_constructible_v<MusicPlaylistRepository>);
static_assert(!std::is_move_assignable_v<MusicPlaylistRepository>);
static_assert(!std::is_copy_constructible_v<ScoreRepository>);
static_assert(!std::is_copy_constructible_v<ReplayRepository>);
static_assert(std::is_constructible_v<result_persistence::Coordinator,
                                      ScoreRepository &,
                                      ReplayRepository &>);
static_assert(requires(ScoreRepository &scores, ReplayRepository &replays) {
  app_database_initializer::initializeApplicationDatabases(scores, replays);
});

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                      \
    return 1;                                                                 \
  }

#define ASSERT_FALSE(value, label)                                             \
  if (value) {                                                                \
    std::cerr << label << " expected false" << std::endl;                     \
    return 1;                                                                 \
  }

int main() {
  bool chartCalled = false;
  bool scoreCalled = false;
  bool replayCalled = false;
  bool musicCalled = false;

  auto success = app_database_initializer::initializeApplicationDatabasesWith(
      [&] {
        chartCalled = true;
        return true;
      },
      [&] {
        scoreCalled = true;
        return true;
      },
      [&] {
        replayCalled = true;
        return true;
      },
      [&] {
        musicCalled = true;
        return true;
      });

  ASSERT_TRUE(chartCalled, "chart initializer called");
  ASSERT_TRUE(scoreCalled, "score initializer called");
  ASSERT_TRUE(replayCalled, "replay initializer called");
  ASSERT_TRUE(musicCalled, "music initializer called");
  ASSERT_TRUE(success.chart, "chart success status");
  ASSERT_TRUE(success.score, "score success status");
  ASSERT_TRUE(success.replay, "replay success status");
  ASSERT_TRUE(success.music, "music success status");
  ASSERT_TRUE(success.ok(), "overall success");

  struct Case {
    const char *label;
    app_database_initializer::DatabaseInitializationStatus expected;
  };
  const Case cases[]{
      {"chart", {.chart = false, .score = true, .replay = true, .music = true}},
      {"score", {.chart = true, .score = false, .replay = true, .music = true}},
      {"replay", {.chart = true, .score = true, .replay = false, .music = true}},
      {"music", {.chart = true, .score = true, .replay = true, .music = false}},
      {"score-replay",
       {.chart = true, .score = false, .replay = false, .music = true}},
  };

  for (const Case &testCase : cases) {
    int chartCalls = 0;
    int scoreCalls = 0;
    int replayCalls = 0;
    int musicCalls = 0;
    const auto result =
        app_database_initializer::initializeApplicationDatabasesWith(
            [&] {
              ++chartCalls;
              return testCase.expected.chart;
            },
            [&] {
              ++scoreCalls;
              return testCase.expected.score;
            },
            [&] {
              ++replayCalls;
              return testCase.expected.replay;
            },
            [&] {
              ++musicCalls;
              return testCase.expected.music;
            });
    ASSERT_TRUE(chartCalls == 1, "chart initializer called once");
    ASSERT_TRUE(scoreCalls == 1, "score initializer called once");
    ASSERT_TRUE(replayCalls == 1, "replay initializer called once");
    ASSERT_TRUE(musicCalls == 1, "music initializer called once");
    ASSERT_TRUE(result.chart == testCase.expected.chart,
                "chart status retained");
    ASSERT_TRUE(result.score == testCase.expected.score,
                "score status retained");
    ASSERT_TRUE(result.replay == testCase.expected.replay,
                "replay status retained");
    ASSERT_TRUE(result.music == testCase.expected.music,
                "music status retained");
    ASSERT_FALSE(result.ok(), "failure aggregate is not ready");
  }

  return 0;
}
