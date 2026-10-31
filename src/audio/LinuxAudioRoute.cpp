#include "../targets.h"
#if TARGET_OS_LINUX
#include "LinuxAudioRoute.h"
#include "AudioRouteGraph.h"
#include <pipewire/pipewire.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace audio {
namespace {
struct Object {
  std::string type;
  std::map<std::string, std::string> properties;
  pw_proxy *proxy = nullptr;
  spa_hook listener{};
  uint32_t input = SPA_ID_INVALID, output = SPA_ID_INVALID;
  void update(const spa_dict *props) {
    if (!props) return;
    const spa_dict_item *item;
    spa_dict_for_each(item, props) {
      if (item->value) properties[item->key] = item->value;
      else properties.erase(item->key);
    }
  }
  std::string get(const char *key) const {
    const auto found = properties.find(key);
    return found == properties.end() ? std::string{} : found->second;
  }
};
struct Graph {
  pw_main_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook registryListener{}, coreListener{};
  std::map<uint32_t, std::unique_ptr<Object>> objects;
  int completed = -1;
  bool failed = false;
  ~Graph() {
    for (auto &[id, object] : objects) if (object->proxy) pw_proxy_destroy(object->proxy);
    if (registry) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    if (core) pw_core_disconnect(core);
    if (context) pw_context_destroy(context);
    if (loop) pw_main_loop_destroy(loop);
  }
};
const pw_node_events nodeEvents{.version = PW_VERSION_NODE_EVENTS,
    .info = [](void *data, const pw_node_info *info) { static_cast<Object *>(data)->update(info->props); }};
const pw_device_events deviceEvents{.version = PW_VERSION_DEVICE_EVENTS,
    .info = [](void *data, const pw_device_info *info) { static_cast<Object *>(data)->update(info->props); }};
const pw_client_events clientEvents{.version = PW_VERSION_CLIENT_EVENTS,
    .info = [](void *data, const pw_client_info *info) { static_cast<Object *>(data)->update(info->props); }};
const pw_link_events linkEvents{.version = PW_VERSION_LINK_EVENTS,
    .info = [](void *data, const pw_link_info *info) {
      auto &object = *static_cast<Object *>(data);
      object.input = info->input_node_id;
      object.output = info->output_node_id;
    }};
const pw_registry_events registryEvents{.version = PW_VERSION_REGISTRY_EVENTS,
    .global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t version,
                 const spa_dict *props) {
      auto &graph = *static_cast<Graph *>(data);
      const std::string kind(type);
      uint32_t bindVersion = 0;
      if (kind == PW_TYPE_INTERFACE_Node) bindVersion = PW_VERSION_NODE;
      else if (kind == PW_TYPE_INTERFACE_Device) bindVersion = PW_VERSION_DEVICE;
      else if (kind == PW_TYPE_INTERFACE_Client) bindVersion = PW_VERSION_CLIENT;
      else if (kind == PW_TYPE_INTERFACE_Link) bindVersion = PW_VERSION_LINK;
      else return;
      auto object = std::make_unique<Object>();
      object->type = kind;
      object->update(props);
      object->proxy = static_cast<pw_proxy *>(pw_registry_bind(
          graph.registry, id, type, std::min(version, bindVersion), 0));
      if (!object->proxy) { graph.failed = true; return; }
      if (kind == PW_TYPE_INTERFACE_Node)
        pw_node_add_listener(reinterpret_cast<pw_node *>(object->proxy), &object->listener, &nodeEvents, object.get());
      else if (kind == PW_TYPE_INTERFACE_Device)
        pw_device_add_listener(reinterpret_cast<pw_device *>(object->proxy), &object->listener, &deviceEvents, object.get());
      else if (kind == PW_TYPE_INTERFACE_Client)
        pw_client_add_listener(reinterpret_cast<pw_client *>(object->proxy), &object->listener, &clientEvents, object.get());
      else
        pw_link_add_listener(reinterpret_cast<pw_link *>(object->proxy), &object->listener, &linkEvents, object.get());
      graph.objects[id] = std::move(object);
    },
    .global_remove = [](void *data, uint32_t) { static_cast<Graph *>(data)->failed = true; }};
const pw_core_events coreEvents{.version = PW_VERSION_CORE_EVENTS,
    .done = [](void *data, uint32_t, int seq) { static_cast<Graph *>(data)->completed = seq; },
    .error = [](void *data, uint32_t, int, int, const char *) { static_cast<Graph *>(data)->failed = true; }};
}

std::optional<bool> linuxProcessOutputIsBluetooth() {
  // Query PipeWire's native graph: JACK clients are not Pulse sink inputs.
  // Follow actual links from this process and use device.api/device.bus,
  // never a friendly name or the system's default-but-unused output.
  static std::once_flag initialized;
  std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
  Graph graph;
  graph.loop = pw_main_loop_new(nullptr);
  if (!graph.loop) return std::nullopt;
  graph.context = pw_context_new(pw_main_loop_get_loop(graph.loop), nullptr, 0);
  if (!graph.context) return std::nullopt;
  graph.core = pw_context_connect(graph.context, nullptr, 0);
  if (!graph.core) return std::nullopt;
  pw_core_add_listener(graph.core, &graph.coreListener, &coreEvents, &graph);
  graph.registry = pw_core_get_registry(graph.core, PW_VERSION_REGISTRY, 0);
  if (!graph.registry) return std::nullopt;
  pw_registry_add_listener(graph.registry, &graph.registryListener, &registryEvents, &graph);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  // First round trip discovers globals; the second receives bound-object info.
  for (int round = 0; round < 2; ++round) {
    const int seq = pw_core_sync(graph.core, PW_ID_CORE, 0);
    if (seq < 0) return std::nullopt;
    while (!graph.failed && graph.completed != seq) {
      if (std::chrono::steady_clock::now() >= deadline ||
          pw_loop_iterate(pw_main_loop_get_loop(graph.loop), 10) < 0) return std::nullopt;
    }
    if (graph.failed) return std::nullopt;
  }
  const auto pid = std::to_string(getpid());
  std::set<std::string> clients;
  std::map<std::string, const Object *> devices;
  for (const auto &[id, object] : graph.objects) {
    if (object->type == PW_TYPE_INTERFACE_Client && object->get("application.process.id") == pid)
      clients.insert(std::to_string(id));
    if (object->type == PW_TYPE_INTERFACE_Device) devices[std::to_string(id)] = object.get();
  }
  std::vector<RouteNode> nodes;
  std::vector<std::pair<uint32_t, uint32_t>> links;
  for (const auto &[id, object] : graph.objects) {
    if (object->type == PW_TYPE_INTERFACE_Link && object->input != SPA_ID_INVALID &&
        object->output != SPA_ID_INVALID) links.emplace_back(object->output, object->input);
    if (object->type != PW_TYPE_INTERFACE_Node) continue;
    const auto &node = *object;
    RouteNode route{.id = id,
        .owned = node.get("application.process.id") == pid || clients.contains(node.get("client.id")),
        .sink = node.get("media.class") == "Audio/Sink"};
    std::string api = node.get("device.api"), bus = node.get("device.bus");
    if (const auto device = devices.find(node.get("device.id")); device != devices.end()) {
      if (const auto value = device->second->get("device.api"); !value.empty()) api = value;
      if (const auto value = device->second->get("device.bus"); !value.empty()) bus = value;
    }
    if (bus == "bluetooth" || api == "bluez5" || api == "bluez") route.bluetooth = true;
    else if (api == "alsa") route.bluetooth = false;
    nodes.push_back(route);
  }
  return routeGraphBluetooth(nodes, links);
}
}
#endif
