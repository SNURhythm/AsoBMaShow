# Strict Legacy RANDOM Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make schema-v10 replay migration reject any `random_values` storage that the retired writer could not have produced, without changing or partially consuming legacy data.

**Architecture:** Keep the parser private to the one-time schema-v10 migration. Read SQLite bytes and storage class exactly, parse canonical decimal `int` tokens into a temporary vector with `std::from_chars`, and publish only after the complete field succeeds.

**Tech Stack:** C++20, SQLite, `std::from_chars`, CMake/CTest, BRD replay codec.

## Global Constraints

- SQL NULL alone represents an empty RANDOM vector.
- Nonempty TEXT grammar is exactly `int ("," int)*`, where `int` is `"0" | "-"? [1-9][0-9]*` and fits `int`.
- Empty TEXT, empty tokens, whitespace, plus signs, leading zeroes, `-0`, malformed tokens, overflow, embedded NUL, and non-TEXT non-NULL storage reject the whole migration.
- Parse into temporary storage; never expose an accepted prefix.
- On rejection, retain schema version 10 and the source value's exact SQLite type/bytes, create no final replay file, and allow a repaired retry.
- Do not reply to or resolve GitHub review threads.

---

### Task 1: Replace permissive RANDOM parsing with an atomic legacy reader

**Files:**

- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp:226-249`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp:852-854`
- Modify: `tests/replay_file_migration_tests.cpp`

**Interfaces:**

- Produces: private `bool readLegacyRandomValues(sqlite3_stmt *statement, int column, std::vector<int> &output, std::string &diagnostic)`.
- Consumes: SQLite NULL or canonical TEXT from column 15 of the schema-v10 replay query.

- [ ] **Step 1: Add success and atomic-rejection fixtures**

Add a helper that updates a fixture row with an explicitly bound SQLite value,
then records `typeof(random_values)` and `hex(random_values)` before migration.
Add table-driven cases for:

```cpp
struct InvalidRandomCase {
  std::string name;
  std::function<void(sqlite3_stmt *)> bind;
};

const std::vector<InvalidRandomCase> invalidCases = {
    {"malformed middle", bindText("1,bad,2")},
    {"positive overflow", bindText("1,2147483648,2")},
    {"negative overflow", bindText("1,-2147483649,2")},
    {"empty text", bindText("")},
    {"leading comma", bindText(",1")},
    {"trailing comma", bindText("1,")},
    {"double comma", bindText("1,,2")},
    {"leading space", bindText(" 1")},
    {"trailing space", bindText("1 ")},
    {"plus sign", bindText("+1")},
    {"leading zero", bindText("01")},
    {"negative zero", bindText("-0")},
    {"integer storage", bindInteger(1)},
    {"blob storage", bindBlob("1,2")},
    {"embedded nul", bindTextBytes({'1', '\0', ',', '2'})},
};
```

For every case assert `MigrationStatus::InvalidLegacyData`, zero chart/course
files, `PRAGMA user_version == 10`, unchanged type and hex bytes, and no replay
directory. Repair the value to canonical `3,4`, rerun, and assert successful
migration plus decoded BRD values `{3, 4}`.

Add valid cases for SQL NULL producing an empty vector and canonical boundary
TEXT `-2147483648,0,2147483647` producing those exact three values.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```sh
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_migration_tests$' --output-on-failure
```

Expected: malformed/noncanonical cases migrate successfully or yield a partial
vector under the current `parseIntegers` implementation.

- [ ] **Step 3: Implement the strict length-aware reader**

Replace `parseIntegers` with a helper shaped as follows:

```cpp
bool readLegacyRandomValues(sqlite3_stmt *statement, int column,
                            std::vector<int> &output,
                            std::string &diagnostic) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    output.clear();
    return true;
  }
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
    diagnostic = "legacy replay RANDOM values have invalid storage";
    return false;
  }

  const auto bytes = sqliteColumnTextView(statement, column);
  if (bytes.empty()) {
    diagnostic = "legacy replay RANDOM values are malformed";
    return false;
  }

  std::vector<int> parsed;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    const std::size_t end = bytes.find(',', begin);
    const std::string_view token = bytes.substr(
        begin, end == std::string_view::npos ? bytes.size() - begin
                                             : end - begin);
    int value = 0;
    const auto [tail, error] =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || error != std::errc{} ||
        tail != token.data() + token.size() ||
        std::to_string(value) != token) {
      diagnostic = "legacy replay RANDOM values are malformed";
      return false;
    }
    parsed.push_back(value);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
    if (begin == bytes.size()) {
      diagnostic = "legacy replay RANDOM values are malformed";
      return false;
    }
  }
  output = std::move(parsed);
  return true;
}
```

Use `sqliteColumnTextView` from `src/repositories/SqliteRAII.h`; it preserves
the byte count reported by `sqlite3_column_bytes`, including embedded NUL.
Add `<charconv>` and `<system_error>`. In `readCharts`, call the helper for
every row and return false immediately on failure.

- [ ] **Step 4: Run focused GREEN and mutation checks**

Run the Step 2 commands. Then temporarily change the full-consumption check to
accept `tail != end` and confirm the malformed-middle/suffix regression fails;
restore the check and rerun. Run `git diff --check`.

- [ ] **Step 5: Commit the strict migration boundary**

```sh
git add src/repositories/ReplayRepositoryReplayFileMigration.cpp tests/replay_file_migration_tests.cpp
git commit -m "fix: reject malformed legacy replay random values"
```
