// Shared runtime fixture for the Route Fabric tests.
#ifndef ROUTEFABRIC_TEST_HARNESS_HPP
#define ROUTEFABRIC_TEST_HARNESS_HPP

#include <memory>
#include <string>

#include "routefabric/backend.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/runtime.hpp"

namespace rftest {

using namespace routefabric;

// Deterministic 128-bit identity helper: "0000...0001", "0000...0002", ...
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
  const Expected<Id> parsed = Id::parse(hex_id(value));
  return parsed.value();
}

struct Harness {
  SyntheticProgrammingBackend backend{BackendId::parse("synthetic-test").value()};
  SyntheticPathAuthority path_authority;
  std::unique_ptr<RouteFabricRuntime> runtime;
  FabricId fabric = FabricId::parse("fabric").value();
  PublisherId publisher = PublisherId::parse("publisher-a").value();
  WorkerBootId boot = make_id<WorkerBootId>(1);
  RoutingNamespace routing_namespace = RoutingNamespace::parse("default").value();
  std::uint64_t attempt_counter = 0;
  std::uint64_t next_hop_counter = 0;

  static std::unique_ptr<Harness> Create(RuntimeConfig config = RuntimeConfig(), bool open = true) {
    auto harness = std::make_unique<Harness>();
    config.fabric = harness->fabric;
    harness->runtime = std::make_unique<RouteFabricRuntime>(config, &harness->backend, &harness->path_authority);
    if (open) {
      const Status status = harness->runtime->Open();
      (void)status;
    }
    return harness;
  }

  PublisherScope scope(bool administrative = false) const {
    PublisherScope result;
    result.fabric = fabric;
    result.wildcard_namespaces = true;
    result.wildcard_destinations = true;
    result.wildcard_route_classes = true;
    result.administrative_override = administrative;
    return result;
  }

  Status register_publisher(const PublisherId& id, const WorkerBootId& worker_boot, bool administrative = false) {
    const Expected<PublisherRegistration> registration =
        runtime->RegisterPublisher(id, worker_boot, scope(administrative));
    if (!registration) {
      return registration.error();
    }
    return ok_status();
  }

  Status register_default(bool administrative = false) {
    return register_publisher(publisher, boot, administrative);
  }

  MutationAttemptId next_attempt() { return make_id<MutationAttemptId>(++attempt_counter); }

  NextHopId next_next_hop() { return make_id<NextHopId>(++next_hop_counter); }

  RouteKey key(const std::string& destination, RouteClass route_class = RouteClass::Unicast) const {
    RouteKey result;
    result.fabric = fabric;
    result.routing_namespace = routing_namespace;
    result.destination = Destination::parse(destination, 128).value();
    result.route_class = route_class;
    return result;
  }

  RouteBinding next_hop_binding(const NextHopId& next_hop) const {
    RouteBinding binding;
    binding.kind = BindingKind::NextHop;
    binding.next_hop.kind = NextHopKind::DirectEndpoint;
    binding.next_hop.next_hop = next_hop;
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

  PublishRequest publish_request(const RouteKey& route_key, const RouteBinding& binding) {
    PublishRequest request;
    request.publisher = publisher;
    request.worker_boot = boot;
    request.attempt = next_attempt();
    request.key = route_key;
    request.binding = binding;
    request.policy_generation = PolicyGeneration::from_value(1);
    request.reason = "test publication";
    return request;
  }

  Expected<PublishResult> publish(const RouteKey& route_key, const RouteBinding& binding) {
    return runtime->PublishRoute(publish_request(route_key, binding));
  }

  // Publishes a route with a fresh next hop and returns the route identity.
  Expected<PublishResult> publish_route(const std::string& destination, RouteClass route_class = RouteClass::Unicast) {
    return publish(key(destination, route_class), next_hop_binding(next_next_hop()));
  }

  Expected<RouteSnapshot> query(const RouteKey& route_key) const { return runtime->QueryRoute(route_key); }
};

}  // namespace rftest

#endif  // ROUTEFABRIC_TEST_HARNESS_HPP
