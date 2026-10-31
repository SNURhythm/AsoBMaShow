#include "audio/AudioRouteGraph.h"
#include <cassert>

int main() {
  using namespace audio;
  std::vector<RouteNode> nodes{{.id=1, .owned=true},
      {.id=2, .sink=true, .bluetooth=false}, {.id=3, .sink=true, .bluetooth=true},
      {.id=4}, {.id=5, .sink=true}};
  assert(routeGraphBluetooth(nodes, {{1,2}}) == false); // Connected BT is unused.
  assert(routeGraphBluetooth(nodes, {{1,3}}) == true);
  assert(!routeGraphBluetooth(nodes, {}));
  assert(!routeGraphBluetooth(nodes, {{1,2}, {1,3}})); // Mixed outputs.
  assert(routeGraphBluetooth(nodes, {{1,4}, {4,3}}) == true); // Filter in path.
  assert(!routeGraphBluetooth(nodes, {{1,5}})); // Unknown virtual sink.
  assert(!routeGraphBluetooth(nodes, {{1,2}, {1,5}}));
  assert(routeGraphBluetooth(nodes, {{1,5}, {5,3}}) == true);
  assert(!routeGraphBluetooth(nodes, {{1,4}, {4,1}})); // Cycle terminates.
  assert(!routeGraphBluetooth(nodes, {{1,999}})); // Removed target.
}
