#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "application startup gate audit failed: $*" >&2
  exit 1
}

match_count() {
  local relative_file=$1
  local pattern=$2
  PATTERN="$pattern" perl -0777 -ne '
    BEGIN { $pattern = $ENV{"PATTERN"}; $count = 0; }
    $count += () = /$pattern/g;
    END { print "$count\n"; }
  ' "$repository_root/$relative_file"
}

require_count() {
  local relative_file=$1
  local pattern=$2
  local expected=$3
  local invariant=$4
  local actual
  actual=$(match_count "$relative_file" "$pattern")
  if [[ "$actual" != "$expected" ]]; then
    fail "$invariant: expected $expected match(es), found $actual"
  fi
}

require_order() {
  local relative_file=$1
  local first=$2
  local second=$3
  local third=$4
  local invariant=$5
  FIRST="$first" SECOND="$second" THIRD="$third" perl -0777 -ne '
    $first = index($_, $ENV{"FIRST"});
    $second = index($_, $ENV{"SECOND"});
    $third = index($_, $ENV{"THIRD"});
    exit(($first >= 0 && $first < $second && $second < $third) ? 0 : 1);
  ' "$repository_root/$relative_file" || fail "$invariant"
}

require_count src/main.h 'int\s+run\(\);' 1 \
  "failure-capable run declaration"
require_count src/main.h 'void\s+run\(\);' 0 \
  "obsolete void run declaration"
require_count src/main.cpp '#include\s+"ApplicationStartup\.h"' 1 \
  "startup coordinator include"
require_count src/main.cpp '#include\s+"ApplicationResultRecovery\.h"' 1 \
  "result recovery coordinator include"
require_count src/main.cpp 'application_startup::execute\s*\(' 1 \
  "single startup readiness owner"
require_count src/main.cpp 'application_result_recovery::execute\s*\(' 1 \
  "single post-database result recovery owner"
require_count src/main.cpp 'int\s+run\(\)' 1 \
  "single failure-capable run definition"
require_count src/main.cpp \
  'int\s+run\(\)\s*\{\s*ApplicationContext\s+context;\s*return\s+application_startup::execute\(\s*context\.profileReady\(\),\s*application_startup::Dependencies\s*\{\s*\.initializeDatabases\s*=\s*\[\]\s*\{\s*return\s+app_database_initializer::initializeApplicationDatabases\(\);\s*\},\s*\.reportFatal\s*=\s*\[&context\]\(const\s+application_startup::Result\s*&result\)\s*\{\s*reportStartupFailure\(context,\s*result\);\s*\},\s*\.runReadyApplication\s*=\s*\[&context\]\s*\{\s*runReadyApplication\(context\);\s*\},\s*\}\);\s*\}' \
  1 "profile predicate and all callbacks are bound inside the gate"
require_count src/main.cpp 'runReadyApplication\(context\)' 1 \
  "result recovery wrapper is reachable only through the startup gate"
require_count src/main.cpp 'runReadyApplicationAfterResultRecovery\(context\)' 1 \
  "ready runtime body is reachable only through result recovery"
require_count src/main.cpp \
  'const\s+int\s+runExitCode\s*=\s*run\(\);[\s\S]{0,500}?return\s+runExitCode\s*;' \
  1 "run exit propagation after renderer cleanup"
require_count src/main.cpp \
  'static\s+void\s+runReadyApplication\(ApplicationContext\s*&context\)' 1 \
  "post-database result recovery wrapper"
require_count src/main.cpp \
  'static\s+void\s+runReadyApplicationAfterResultRecovery\(ApplicationContext\s*&context\)' 1 \
  "post-recovery ready-only runtime body"
require_count src/main.cpp 'SceneManager\s+sceneManager\(context\)' 1 \
  "single ready runtime scene manager"
require_count src/main.cpp \
  'app_database_initializer::initializeApplicationDatabases\(\)' 1 \
  "single injected database initialization"
require_count src/main.cpp 'SDL_ShowSimpleMessageBox\s*\(' 2 \
  "one native fatal reporter and one native recovery warning"
require_count src/main.cpp '"AsoBMaShow Startup Error"' 1 \
  "exact startup failure title"
require_count src/main.cpp '"AsoBMaShow Result Recovery"' 1 \
  "exact result recovery warning title"
require_count src/main.cpp \
  'SDL_ShowSimpleMessageBox\(SDL_MESSAGEBOX_WARNING,\s*"AsoBMaShow Result Recovery",\s*recovery\.userMessage\.c_str\(\),\s*s_window\)' \
  1 "recovery warning exposes only sanitized user copy on the current window"
require_count src/main.cpp \
  'Unable to show the startup error dialog:[\s\S]{0,120}?SDL_GetError\s*\(\)' 1 \
  "message-box failure logging"
require_count src/main.cpp \
  'Unable to show the result recovery warning:[\s\S]{0,120}?SDL_GetError\s*\(\)' 1 \
  "recovery warning failure logging"
require_count src/main.cpp \
  'Application startup stopped because|if\s*\(!databaseStatus\.ok\(\)\)' 0 \
  "obsolete inline readiness branches"
require_count src/context.h 'Player profile initialization failed' 0 \
  "duplicate context startup log"
require_count src/CMakeLists.txt '\bApplicationStartup\.cpp\b' 1 \
  "main target startup source"
require_count src/CMakeLists.txt '\bApplicationResultRecovery\.cpp\b' 1 \
  "main target result recovery source"
require_count ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  '[[:space:]]ApplicationStartup\.cpp,' 1 "iOS startup source membership"
require_count ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  '[[:space:]]ApplicationResultRecovery\.cpp,' 1 \
  "iOS result recovery source membership"
require_order src/main.cpp \
  'runReadyApplicationAfterResultRecovery(ApplicationContext &context)' \
  'SceneManager sceneManager(context)' \
  'static void runReadyApplication(ApplicationContext &context)' \
  "SceneManager must remain inside the post-recovery runtime body"
require_order src/main.cpp \
  'static void runReadyApplication(ApplicationContext &context)' \
  'application_result_recovery::execute(' \
  'int run()' \
  "result recovery wrapper must remain inside the startup gate callback path"

echo "application startup gate audit passed"
