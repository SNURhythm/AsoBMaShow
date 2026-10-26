add_executable(streak_model_tests tests/streak_model_tests.cpp src/streak/StreakModel.cpp)
target_include_directories(streak_model_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(streak_model_tests PRIVATE cxx_std_23)
add_test(NAME streak_model_tests COMMAND streak_model_tests)

add_executable(streak_store_tests tests/streak_store_tests.cpp src/streak/StreakModel.cpp
    src/streak/StreakStore.cpp src/sqlite3.c)
target_include_directories(streak_store_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(streak_store_tests PRIVATE cxx_std_23)
add_test(NAME streak_store_tests COMMAND streak_store_tests)

add_test(NAME streak_scene_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/streak_scene_tests.py -v)
