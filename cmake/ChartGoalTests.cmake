add_executable(chart_goal_tests tests/chart_goal_tests.cpp
    src/goals/ChartGoal.cpp src/goals/ChartGoalStore.cpp src/sqlite3.c)
target_include_directories(chart_goal_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(chart_goal_tests PRIVATE cxx_std_23)
add_test(NAME chart_goal_tests COMMAND chart_goal_tests)

target_sources(ir_score_import_projection_tests PRIVATE src/repositories/ScoreRepositoryGoals.cpp
    src/goals/ChartGoal.cpp src/goals/ChartGoalStore.cpp)
