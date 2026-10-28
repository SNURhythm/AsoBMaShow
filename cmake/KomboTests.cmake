add_executable(kombo_model_tests tests/kombo_model_tests.cpp src/kombo/KomboModel.cpp)
target_include_directories(kombo_model_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(kombo_model_tests PRIVATE cxx_std_23)
add_test(NAME kombo_model_tests COMMAND kombo_model_tests)

add_executable(kombo_store_tests tests/kombo_store_tests.cpp src/kombo/KomboModel.cpp
    src/kombo/KomboStore.cpp src/sqlite3.c)
target_include_directories(kombo_store_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(kombo_store_tests PRIVATE cxx_std_23)
add_test(NAME kombo_store_tests COMMAND kombo_store_tests)

add_test(NAME kombo_scene_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/kombo_scene_tests.py -v)

add_executable(reminder_plan_tests tests/reminder_plan_tests.cpp src/kombo/KomboModel.cpp)
target_include_directories(reminder_plan_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(reminder_plan_tests PRIVATE cxx_std_23)
add_test(NAME reminder_plan_tests COMMAND reminder_plan_tests)
