add_executable(chart_goal_tests tests/chart_goal_tests.cpp
    src/goals/ChartGoal.cpp src/goals/ChartGoalStore.cpp src/sqlite3.c)
target_include_directories(chart_goal_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(chart_goal_tests PRIVATE cxx_std_23)
add_test(NAME chart_goal_tests COMMAND chart_goal_tests)

add_test(NAME chart_goals_scene_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/chart_goals_scene_tests.py -v)

add_executable(folder_goal_tests tests/folder_goal_tests.cpp
    src/goals/FolderGoal.cpp src/goals/FolderGoalStore.cpp src/sqlite3.c)
target_include_directories(folder_goal_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(folder_goal_tests PRIVATE cxx_std_23)
add_test(NAME folder_goal_tests COMMAND folder_goal_tests)

add_test(NAME folder_goals_scene_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/folder_goals_scene_tests.py -v)
