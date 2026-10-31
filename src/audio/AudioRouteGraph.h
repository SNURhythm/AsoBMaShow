#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace audio {
struct RouteNode {
  std::uint32_t id = 0;
  bool owned = false;
  bool sink = false;
  std::optional<bool> bluetooth;
};

// Resolve only outputs reachable from this application's nodes. An unidentified
// terminal sink or mixed transports cannot safely select a timing profile.
inline std::optional<bool> routeGraphBluetooth(
    const std::vector<RouteNode> &nodes,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &links) {
  std::map<std::uint32_t, const RouteNode *> byId;
  std::map<std::uint32_t, std::vector<std::uint32_t>> edges;
  std::vector<std::uint32_t> pending;
  for (const auto &node : nodes) {
    byId[node.id] = &node;
    if (node.owned) pending.push_back(node.id);
  }
  for (const auto &[output, input] : links) edges[output].push_back(input);
  std::optional<bool> result;
  std::set<std::uint32_t> visited;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!visited.insert(id).second) continue;
    const auto found = byId.find(id);
    if (found == byId.end()) return std::nullopt;
    const auto &node = *found->second;
    const auto connections = edges.find(id);
    if (connections != edges.end())
      pending.insert(pending.end(), connections->second.begin(), connections->second.end());
    if (!node.sink) continue;
    if (node.bluetooth) {
      if (result && result != node.bluetooth) return std::nullopt;
      result = node.bluetooth;
    } else if (connections == edges.end()) return std::nullopt;
  }
  return result;
}
}
