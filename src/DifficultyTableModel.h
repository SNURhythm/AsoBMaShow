#pragma once

#include <optional>
#include <string>
#include <vector>

namespace difficulty_table {

struct Chart {
  std::string level;
  std::string md5;
  std::string sha256;
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subartist;
  std::string url;
  std::string urlDiff;
  std::optional<std::vector<std::string>> originalMd5s;
};

struct Course {
  std::string name;
  std::string groupName;
  std::string level;
  std::string constraintJson;
  std::vector<Chart> charts;
};

struct Document {
  std::string name;
  std::string symbol;
  std::string sourceUrl;
  std::string dataUrl;
  std::vector<std::string> levelOrder;
  std::vector<Chart> charts;
  std::vector<Course> courses;
};

std::optional<Document> Parse(const std::string &headerJson,
                              const std::string &dataJson,
                              const std::string &sourceUrl,
                              std::string &errorMessage);

} // namespace difficulty_table
