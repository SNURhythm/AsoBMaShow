#include "../src/AppDatabaseInitializer.h"

#include <iostream>

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

  chartCalled = false;
  scoreCalled = false;
  replayCalled = false;
  musicCalled = false;

  auto partialFailure =
      app_database_initializer::initializeApplicationDatabasesWith(
          [&] {
            chartCalled = true;
            return true;
          },
          [&] {
            scoreCalled = true;
            return false;
          },
          [&] {
            replayCalled = true;
            return true;
          },
          [&] {
            musicCalled = true;
            return true;
          });

  ASSERT_TRUE(chartCalled, "chart initializer called after failure");
  ASSERT_TRUE(scoreCalled, "score initializer called after failure");
  ASSERT_TRUE(replayCalled, "replay initializer called after failure");
  ASSERT_TRUE(musicCalled, "music initializer called after failure");
  ASSERT_TRUE(partialFailure.chart, "partial chart success status");
  ASSERT_FALSE(partialFailure.score, "partial score failure status");
  ASSERT_TRUE(partialFailure.replay, "partial replay success status");
  ASSERT_TRUE(partialFailure.music, "partial music success status");
  ASSERT_FALSE(partialFailure.ok(), "overall partial failure");

  return 0;
}
