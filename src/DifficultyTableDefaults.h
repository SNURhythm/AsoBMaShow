#pragma once

namespace difficulty_table {

struct DefaultSource {
  int seedRevision;
  const char *url;
};

// Add new sources at a new revision; never renumber existing entries.
inline constexpr int kBundledSeedRevision = 2;
inline constexpr DefaultSource kDefaultSources[] = {
    {1, "https://rattoto10.jounin.jp/table.html"},
    {1, "https://rattoto10.jounin.jp/table_insane.html"},
    {1, "https://miraiscarlet.github.io/bms/table/genocide_normal/normal_bms.html"},
    {1, "https://miraiscarlet.github.io/bms/table/genocide_insane/insane_bms.html"},
    {1, "https://stellabms.xyz/sl/table.html"},
    {1, "https://stellabms.xyz/st/table.html"},
    {2, "https://asumatoki.kr/table/aery/header.json"},
    {2, "https://asumatoki.kr/table/aery7/header.json"},
};

} // namespace difficulty_table
