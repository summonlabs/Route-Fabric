// Shared helpers for the Route Fabric examples. Examples use only the public
// library API.
#ifndef ROUTEFABRIC_EXAMPLE_SUPPORT_HPP
#define ROUTEFABRIC_EXAMPLE_SUPPORT_HPP

#include <iostream>
#include <string>

#include "routefabric/backend.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/runtime.hpp"

namespace example {

using namespace routefabric;

inline void say(const std::string& text) { std::cout << text << '\n'; }

inline bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cout << "example failed: " << message << '\n';
    return false;
  }
  return true;
}

inline std::string hex_id(std::uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int index = 0; index < 16; ++index) {
    out[31 - index] = kDigits[(value >> (4 * index)) & 0x0Fu];
  }
  return out;
}

template <typename Id>
inline Id make_id(std::uint64_t value) {
  return Id::parse(hex_id(value)).value();
}

// A ready-made runtime with a synthetic programming backend and a synthetic path
// authority, both labelled where they are reported.
struct ExampleRuntime {
  SyntheticProgrammingBackend backend{BackendId::parse("synthetic-example").value()};
  SyntheticPathAuthority path_authority;
  std::unique_ptr<RouteFabricRuntime> runtime;
  FabricId fabric = FabricId::parse("fabric").value();
  PublisherId publisher = PublisherId::parse("example-publisher").value();
  WorkerBootId boot = make_id<WorkerBootId>(1);

  static std::unique_ptr<ExampleRuntime> Create(RuntimeConfig config = RuntimeConfig(), bool open = true) {
    auto instance = std::make_unique<ExampleRuntime>();
    config.fabric = instance->fabric;
    instance->runtime = std::make_unique<RouteFabricRuntime>(config, &instance->backend, &instance->path_authority);
    if (open) {
      const Status status = instance->runtime->Open();
      if (!status) {
        say(std::string("open failed: ") + status.error().detail());
      }
    }
    return instance;
  }

  PublisherScope scope() const {
    PublisherScope result;
    result.fabric = fabric;
    result.wildcard_namespaces = true;
    result.wildcard_destinations = true;
    result.wildcard_route_classes = true;
    return result;
  }

  bool register_publisher(const PublisherId& id, const WorkerBootId& worker_boot) {
    const Expected<PublisherRegistration> registration = runtime->RegisterPublisher(id, worker_boot, scope());
    return registration.has_value();
  }

  RouteKey key(const std::string& destination, RouteClass route_class = RouteClass::Unicast) const {
    RouteKey result;
    result.fabric = fabric;
    result.routing_namespace = RoutingNamespace::parse("default").value();
    result.destination = Destination::parse(destination, 128).value();
    result.route_class = route_class;
    return result;
  }

  RouteBinding next_hop_binding(std::uint64_t seed) const {
    RouteBinding binding;
    binding.kind = BindingKind::NextHop;
    binding.next_hop.kind = NextHopKind::DirectEndpoint;
    binding.next_hop.next_hop = make_id<NextHopId>(seed);
    binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
    binding.policy_generation = PolicyGeneration::from_value(1);
    return binding;
  }

  RouteBinding path_binding(const PathId& path, const PathAuthorityGeneration& generation) const {
    RouteBinding binding;
    binding.kind = BindingKind::AuthorizedPath;
    binding.path = path;
    binding.path_authority_generation = generation;
    binding.policy_generation = PolicyGeneration::from_value(1);
    binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
    return binding;
  }

  PublishRequest publish_request(const RouteKey& route_key, const RouteBinding& binding,
                                 std::uint64_t attempt = 1) const {
    PublishRequest request;
    request.publisher = publisher;
    request.worker_boot = boot;
    request.attempt = make_id<MutationAttemptId>(attempt);
    request.key = route_key;
    request.binding = binding;
    request.policy_generation = PolicyGeneration::from_value(1);
    request.reason = "example publication";
    return request;
  }
};

}  // namespace example

#endif  // ROUTEFABRIC_EXAMPLE_SUPPORT_HPP
