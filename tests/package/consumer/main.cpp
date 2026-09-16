// Independent downstream consumer.
//
// Uses only the installed Route Fabric package: find_package(RouteFabric CONFIG),
// link SummonSoftwareLabs::RouteFabric, build a runtime, publish a route, inspect
// it, replace it, withdraw it, and observe the expected lifecycle.
#include <iostream>
#include <memory>
#include <string>

#include "routefabric/backend.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/version.hpp"

namespace {

std::string hex_id(std::uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int index = 0; index < 16; ++index) {
    out[31 - index] = kDigits[(value >> (4 * index)) & 0x0Fu];
  }
  return out;
}

template <typename Id>
Id make_id(std::uint64_t value) {
  return Id::parse(hex_id(value)).value();
}

}  // namespace

int main() {
  using namespace routefabric;
  std::cout << "route-fabric-package version " << kVersionString << '\n';

  SyntheticProgrammingBackend backend(BackendId::parse("synthetic-consumer").value());
  SyntheticPathAuthority path_authority;
  RuntimeConfig config;
  config.fabric = FabricId::parse("consumer-fabric").value();
  RouteFabricRuntime runtime(config, &backend, &path_authority);
  if (!runtime.Open()) {
    std::cout << "consumer failed: cannot open the runtime\n";
    return 1;
  }

  PublisherScope scope;
  scope.fabric = config.fabric;
  scope.wildcard_namespaces = true;
  scope.wildcard_destinations = true;
  scope.wildcard_route_classes = true;
  const PublisherId publisher = PublisherId::parse("consumer-publisher").value();
  const WorkerBootId boot = make_id<WorkerBootId>(1);
  if (!runtime.RegisterPublisher(publisher, boot, scope)) {
    std::cout << "consumer failed: cannot register the publisher\n";
    return 1;
  }

  RouteKey key;
  key.fabric = config.fabric;
  key.routing_namespace = RoutingNamespace::parse("default").value();
  key.destination = Destination::parse_ipv4_prefix("10.0.0.0/24").value();
  key.route_class = RouteClass::Unicast;

  RouteBinding binding;
  binding.kind = BindingKind::NextHop;
  binding.next_hop.kind = NextHopKind::DirectEndpoint;
  binding.next_hop.next_hop = make_id<NextHopId>(1);
  binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
  binding.policy_generation = PolicyGeneration::from_value(1);

  PublishRequest request;
  request.publisher = publisher;
  request.worker_boot = boot;
  request.attempt = make_id<MutationAttemptId>(1);
  request.key = key;
  request.binding = binding;
  request.policy_generation = PolicyGeneration::from_value(1);

  const Expected<PublishResult> published = runtime.PublishRoute(request);
  if (!published) {
    std::cout << "consumer failed: publication rejected\n";
    return 1;
  }
  std::cout << "published " << published.value().route.render() << " lifecycle "
            << to_string(published.value().lifecycle) << " currentness "
            << to_string(published.value().currentness) << '\n';
  if (published.value().lifecycle != RouteLifecycle::Installed ||
      published.value().currentness != RouteCurrentness::Current) {
    std::cout << "consumer failed: unexpected lifecycle after publication\n";
    return 1;
  }

  const Expected<RouteSnapshot> snapshot = runtime.QueryRoute(key);
  if (!snapshot) {
    std::cout << "consumer failed: query\n";
    return 1;
  }
  std::cout << "queried generation " << snapshot.value().record.generation.render() << " digest "
            << snapshot.value().digest.to_hex() << '\n';

  RouteBinding replacement = binding;
  replacement.next_hop.next_hop = make_id<NextHopId>(2);
  PublishRequest replace_request = request;
  replace_request.attempt = make_id<MutationAttemptId>(2);
  replace_request.binding = replacement;
  const Expected<PublishResult> replaced = runtime.PublishRoute(replace_request);
  if (!replaced || replaced.value().generation.value() != 2) {
    std::cout << "consumer failed: replacement\n";
    return 1;
  }
  std::cout << "replaced generation " << replaced.value().generation.render() << '\n';

  WithdrawRequest withdraw;
  withdraw.publisher = publisher;
  withdraw.worker_boot = boot;
  withdraw.attempt = make_id<MutationAttemptId>(3);
  withdraw.route = replaced.value().route;
  const Expected<WithdrawOutcome> withdrawn = runtime.WithdrawRoute(withdraw);
  if (!withdrawn || withdrawn.value().lifecycle != RouteLifecycle::Withdrawn) {
    std::cout << "consumer failed: withdrawal\n";
    return 1;
  }
  std::cout << "withdrawn lifecycle " << to_string(withdrawn.value().lifecycle) << '\n';
  std::cout << "consumer ok\n";
  return 0;
}
