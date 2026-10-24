# Keep the Swift bridge outside directory-wide C/C++ flags and definitions.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "" COMPILE_DEFINITIONS "" INCLUDE_DIRECTORIES "")
add_library(asobmashow_readme_translation STATIC
    ${PROJECT_SOURCE_DIR}/src/platform/ReadmeTranslationApple.swift
    ${PROJECT_SOURCE_DIR}/src/platform/ReadmeTranslationOutput.swift)
set_target_properties(asobmashow_readme_translation PROPERTIES Swift_LANGUAGE_VERSION 5)

if(ASOBMASHOW_BUILD_TESTS AND BUILD_TESTING)
    add_executable(readme_translation_apple_tests
        ${PROJECT_SOURCE_DIR}/tests/readme_translation_apple_tests.swift
        ${PROJECT_SOURCE_DIR}/src/platform/ReadmeTranslationOutput.swift)
    set_target_properties(readme_translation_apple_tests PROPERTIES Swift_LANGUAGE_VERSION 5)
endif()
