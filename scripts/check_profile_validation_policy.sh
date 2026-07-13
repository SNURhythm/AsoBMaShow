#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
header="$repository_root/src/PlayerProfileManager.h"
source_file="$repository_root/src/PlayerProfileManager.cpp"

fail() {
  echo "profile validation policy audit failed: $*" >&2
  exit 1
}

match_count() {
  local file=$1
  local pattern=$2
  PATTERN="$pattern" perl -0777 -ne '
    BEGIN { $pattern = $ENV{"PATTERN"}; $count = 0; }
    $count += () = /$pattern/g;
    END { print "$count\n"; }
  ' "$file"
}

require_count() {
  local file=$1
  local pattern=$2
  local expected=$3
  local role=$4
  local actual
  actual=$(match_count "$file" "$pattern")
  if [[ "$actual" != "$expected" ]]; then
    fail "$role: expected $expected match(es), found $actual"
  fi
}

if rg -n 'ValidationDepth|DatabaseVersionPolicy' "$header" "$source_file" \
    >/dev/null; then
  fail "obsolete split validation policy names remain"
fi
require_count "$source_file" \
  'validateProfileFiles\([^)]*(?:true|false)\s*\)' 0 \
  "raw boolean validateProfileFiles calls"

require_count "$header" \
  'enum\s+class\s+ProfileUse\s*:\s*unsigned\s+char\s*;' 1 \
  "opaque ProfileUse declaration"
require_count "$source_file" \
  'enum\s+class\s+ProfileUse\s*:\s*unsigned\s+char\s*\{\s*Catalog,\s*Manage,\s*Activate,\s*RuntimeReady,\s*\};' \
  1 "ProfileUse definition"

require_count "$source_file" 'ProfileUse::Catalog' 3 \
  "Catalog enumerator allowlist"
require_count "$source_file" 'ProfileUse::Manage' 13 \
  "Manage enumerator allowlist"
require_count "$source_file" 'ProfileUse::Activate' 5 \
  "Activate enumerator allowlist"
require_count "$source_file" 'ProfileUse::RuntimeReady' 4 \
  "RuntimeReady enumerator allowlist"

require_count "$source_file" \
  'constexpr\s+ProfileValidationPolicy\s+validationPolicy\(ProfileUse\s+use\)\s*\{\s*switch\s*\(use\)\s*\{\s*case\s+ProfileUse::Catalog\s*:\s*return\s*\{\.deep\s*=\s*false,\s*\.allowSupportedOlderDatabases\s*=\s*true\};\s*case\s+ProfileUse::Manage\s*:\s*case\s+ProfileUse::Activate\s*:\s*return\s*\{\.deep\s*=\s*true,\s*\.allowSupportedOlderDatabases\s*=\s*true\};\s*case\s+ProfileUse::RuntimeReady\s*:\s*return\s*\{\.deep\s*=\s*true,\s*\.allowSupportedOlderDatabases\s*=\s*false\};\s*\}\s*return\s*\{\.deep\s*=\s*true,\s*\.allowSupportedOlderDatabases\s*=\s*false\};\s*\}' \
  1 "single exhaustive validationPolicy mapper"

require_count "$source_file" 'ProfileValidationPolicy' 4 \
  "ProfileValidationPolicy declaration and consumers"
require_count "$source_file" '\bdeep\b' 7 \
  "low-level depth choices and consumers"
require_count "$source_file" '\ballowSupportedOlderDatabases\b' 7 \
  "low-level database compatibility choices and consumers"
require_count "$source_file" \
  'validateProfileFiles\([^)]*ProfileUse\s+use\)\s*\{\s*const\s+ProfileValidationPolicy\s+policy\s*=\s*validationPolicy\(use\);' \
  1 "path validator policy consumption"
require_count "$source_file" \
  'PlayerProfileManager::validateProfile\(std::string_view\s+id,\s*ProfileUse\s+use\)\s+const\s*\{\s*const\s+ProfileValidationPolicy\s+policy\s*=\s*validationPolicy\(use\);' \
  1 "canonical validator policy consumption"
require_count "$source_file" 'policy\.deep' 2 \
  "canonical and path depth consumption"
require_count "$source_file" 'policy\.allowSupportedOlderDatabases' 2 \
  "canonical and path database compatibility consumption"

# Catalog: mapper, primary bootstrap admission, and public catalog.
require_count "$source_file" 'case\s+ProfileUse::Catalog\s*:' 1 \
  "Catalog mapper case"
require_count "$source_file" \
  'validateProfile\(bootstrap\.id,\s*ProfileUse::Catalog\)' 1 \
  "Catalog primary bootstrap admission"
require_count "$source_file" \
  'PlayerProfileManager::listProfiles\(\)\s+const\s*\{\s*return\s+listProfiles\(ProfileUse::Catalog\);\s*\}' \
  1 "Catalog public listing"

# Activate: mapper, backup/orphan/future startup recovery, and preflight.
require_count "$source_file" 'case\s+ProfileUse::Activate\s*:' 1 \
  "Activate mapper case"
require_count "$source_file" \
  'validateProfile\(backup\.id,\s*ProfileUse::Activate\)' 1 \
  "Activate bootstrap-backup recovery"
require_count "$source_file" \
  'profiles\s*=\s*listProfiles\(ProfileUse::Activate\)' 1 \
  "Activate orphan recovery listing"
require_count "$source_file" \
  'validateProfile\(candidateId,\s*ProfileUse::Activate\)' 1 \
  "Activate future-profile startup scan"
require_count "$source_file" \
  'validateProfileForActivation\(std::string_view\s+id\)\s+const\s*\{\s*return\s+validateProfile\(id,\s*ProfileUse::Activate\);\s*\}' \
  1 "Activate public preflight"

# Manage: mapper, CRUD, deletion reconciliation, overwrite, and recovery.
require_count "$source_file" 'case\s+ProfileUse::Manage\s*:' 1 \
  "Manage mapper case"
require_count "$source_file" \
  'validateProfile\(sourceId,\s*ProfileUse::Manage\)' 1 \
  "Manage duplicate source"
require_count "$source_file" \
  'PlayerProfileManager::renameProfile[\s\S]{0,1200}?validateProfile\(id,\s*ProfileUse::Manage\)' \
  1 "Manage rename target"
require_count "$source_file" \
  'PlayerProfileManager::deleteProfile[\s\S]{0,1200}?validateProfile\(id,\s*ProfileUse::Manage\)' \
  1 "Manage delete target"
require_count "$source_file" \
  'PlayerProfileManager::deleteProfile[\s\S]{0,1600}?listProfiles\(ProfileUse::Manage\)' \
  1 "Manage delete count"
require_count "$source_file" \
  'validateProfileFiles\(\s*applicationRoot,\s*source,\s*profile\.id,\s*ProfileUse::Manage\)' \
  1 "Manage deletion source reconciliation"
require_count "$source_file" \
  'validateProfileFiles\(\s*applicationRoot,\s*tombstonePaths,\s*profile\.id,\s*ProfileUse::Manage\)' \
  1 "Manage deletion tombstone reconciliation"
require_count "$source_file" \
  'validateProfile\(\*overwriteProfileId,\s*ProfileUse::Manage\)' 1 \
  "Manage overwrite target"
require_count "$source_file" \
  'PlayerProfileManager::installProfile[\s\S]{0,2200}?listProfiles\(ProfileUse::Manage\)' \
  1 "Manage overwrite count"
require_count "$source_file" \
  'validateProfileFiles\(applicationRoot,\s*destination,\s*id,\s*ProfileUse::Manage\)' \
  1 "Manage startup overwrite destination recovery"
require_count "$source_file" \
  'validateProfileFiles\(applicationRoot,\s*backup,\s*id,\s*ProfileUse::Manage\)' \
  2 "Manage startup overwrite backup recovery"
require_count "$source_file" \
  'validateProfileFiles\(\s*applicationDataRoot_,\s*makePathsAtRoot\(backup\),\s*id,\s*ProfileUse::Manage\)' \
  1 "Manage live overwrite rollback backup"

# RuntimeReady: mapper, new destinations, strict public validation, and import.
require_count "$source_file" 'case\s+ProfileUse::RuntimeReady\s*:' 1 \
  "RuntimeReady mapper case"
require_count "$source_file" \
  'validateProfileFiles\(\s*applicationRoot,\s*destination,\s*profile\.id,\s*ProfileUse::RuntimeReady\)' \
  1 "RuntimeReady new-profile destination"
require_count "$source_file" \
  'PlayerProfileManager::validateProfile\(std::string_view\s+id\)\s+const\s*\{\s*return\s+validateProfile\(id,\s*ProfileUse::RuntimeReady\);\s*\}' \
  1 "RuntimeReady public strict validation"
require_count "$source_file" \
  'validateProfileFiles\(\s*applicationDataRoot_,\s*staging,\s*sourceProfile\.id,\s*ProfileUse::RuntimeReady\)' \
  1 "RuntimeReady imported staging validation"

echo "profile validation policy audit passed"
