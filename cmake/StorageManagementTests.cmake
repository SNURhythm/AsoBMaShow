add_executable(storage_policy_tests tests/storage_policy_tests.cpp
    src/storage/StoragePolicy.cpp)
target_compile_features(storage_policy_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_policy_tests)

add_executable(storage_inventory_tests tests/storage_inventory_tests.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp)
target_compile_features(storage_inventory_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_inventory_tests)
