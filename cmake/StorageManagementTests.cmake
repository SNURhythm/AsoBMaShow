add_executable(storage_policy_tests tests/storage_policy_tests.cpp
    src/storage/StoragePolicy.cpp)
target_compile_features(storage_policy_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_policy_tests)
