// Route Fabric benchmarks. Every benchmark reports the number of operations that
// actually completed, so a partial or failed run cannot be mistaken for a fast
// one. The numbers are measurements of this machine and this build; they are not
// portable performance guarantees.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/persistence.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/version.hpp"

using namespace routefabric;

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

struct BenchContext {
  SyntheticProgrammingBackend backend{BackendId::parse("synthetic-bench").value()};
  SyntheticPathAuthority path_authority;
  std::unique_ptr<RouteFabricRuntime> runtime;
  FabricId fabric = FabricId::parse("fabric").value();
  PublisherId publisher = PublisherId::parse("benchmark-publisher").value();
  WorkerBootId boot = make_id<WorkerBootId>(1);
  std::uint64_t attempts = 0;
  std::uint64_t next_hops = 0;

  static std::unique_ptr<BenchContext> Create(RuntimeConfig config = RuntimeConfig()) {
    auto context = std::make_unique<BenchContext>();
    config.fabric = context->fabric;
    context->runtime = std::make_unique<RouteFabricRuntime>(config, &context->backend, &context->path_authority);
    const Status status = context->runtime->Open();
    if (!status) {
      std::cout << "open failed: " << status.error().detail() << '\n';
    }
    PublisherScope scope;
    scope.fabric = context->fabric;
    scope.wildcard_namespaces = true;
    scope.wildcard_destinations = true;
    scope.wildcard_route_classes = true;
    if (context->runtime->RegisterPublisher(context->publisher, context->boot, scope).has_value() == false) {
      std::cout << "registration failed\n";
    }
    return context;
  }

  RouteKey key(std::uint64_t index) const {
    RouteKey result;
    result.fabric = fabric;
    result.routing_namespace = RoutingNamespace::parse("default").value();
    result.destination = Destination::parse_ipv4_prefix("10." + to_decimal((index >> 8) & 0xFFu) + "." +
                                                        to_decimal(index & 0xFFu) + ".0/24")
                             .value();
    result.route_class = RouteClass::Unicast;
    return result;
  }

  RouteBinding binding() {
    RouteBinding result;
    result.kind = BindingKind::NextHop;
    result.next_hop.kind = NextHopKind::DirectEndpoint;
    result.next_hop.next_hop = make_id<NextHopId>(++next_hops);
    result.next_hop.entity_generation = PolicyGeneration::from_value(1);
    result.policy_generation = PolicyGeneration::from_value(1);
    return result;
  }

  PublishRequest request(const RouteKey& route_key, const RouteBinding& route_binding) {
    PublishRequest result;
    result.publisher = publisher;
    result.worker_boot = boot;
    result.attempt = make_id<MutationAttemptId>(++attempts);
    result.key = route_key;
    result.binding = route_binding;
    result.policy_generation = PolicyGeneration::from_value(1);
    return result;
  }
};

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  double milliseconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void report(const std::string& name, std::uint64_t operations, double milliseconds) {
  const double per_operation = operations == 0 ? 0.0 : milliseconds / static_cast<double>(operations);
  std::cout << name << " completed " << operations << " elapsed-ms " << static_cast<std::uint64_t>(milliseconds)
            << " us-per-operation " << static_cast<std::uint64_t>(per_operation * 1000.0) << '\n';
}

std::uint64_t publish_many(std::uint64_t count) {
  auto context = BenchContext::Create();
  std::uint64_t completed = 0;
  Timer timer;
  for (std::uint64_t index = 0; index < count; ++index) {
    const RouteKey key = context->key(index);
    if (context->runtime->PublishRoute(context->request(key, context->binding())).has_value()) {
      ++completed;
    }
  }
  const double elapsed = timer.milliseconds();
  report("publish-" + to_decimal(count), completed, elapsed);
  return completed;
}

void lookup_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  for (std::uint64_t index = 0; index < count; ++index) {
    const RouteKey key = context->key(index);
    if (!context->runtime->PublishRoute(context->request(key, context->binding())).has_value()) {
      std::cout << "lookup benchmark setup failed\n";
      return;
    }
  }
  std::uint64_t completed = 0;
  Timer timer;
  for (std::uint64_t index = 0; index < count; ++index) {
    if (context->runtime->QueryRoute(context->key(index)).has_value()) {
      ++completed;
    }
  }
  report("exact-lookup-" + to_decimal(count), completed, timer.milliseconds());
}

void replacement_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  for (std::uint64_t index = 0; index < count; ++index) {
    const RouteKey key = context->key(index);
    if (!context->runtime->PublishRoute(context->request(key, context->binding())).has_value()) {
      std::cout << "replacement benchmark setup failed\n";
      return;
    }
  }
  std::uint64_t completed = 0;
  Timer timer;
  for (std::uint64_t index = 0; index < count; ++index) {
    const RouteKey key = context->key(index);
    if (context->runtime->PublishRoute(context->request(key, context->binding())).has_value()) {
      ++completed;
    }
  }
  report("replace-" + to_decimal(count), completed, timer.milliseconds());
}

void withdrawal_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  std::vector<RouteId> routes;
  routes.reserve(count);
  for (std::uint64_t index = 0; index < count; ++index) {
    const Expected<PublishResult> published =
        context->runtime->PublishRoute(context->request(context->key(index), context->binding()));
    if (published.has_value()) {
      routes.push_back(published.value().route);
    }
  }
  std::uint64_t completed = 0;
  Timer timer;
  for (const RouteId& route : routes) {
    WithdrawRequest request;
    request.publisher = context->publisher;
    request.worker_boot = context->boot;
    request.attempt = make_id<MutationAttemptId>(++context->attempts);
    request.route = route;
    if (context->runtime->WithdrawRoute(request).has_value()) {
      ++completed;
    }
  }
  report("withdraw-" + to_decimal(count), completed, timer.milliseconds());
}

void path_invalidation_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  const PathId path = make_id<PathId>(1);
  const PathAuthorityGeneration generation = PathAuthorityGeneration::from_value(1);
  (void)context->path_authority.SetPath(path, generation, PathAuthorization::Usable);
  std::uint64_t installed = 0;
  for (std::uint64_t index = 0; index < count; ++index) {
    RouteBinding binding;
    binding.kind = BindingKind::AuthorizedPath;
    binding.path = path;
    binding.path_authority_generation = generation;
    binding.policy_generation = PolicyGeneration::from_value(1);
    binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
    if (context->runtime->PublishRoute(context->request(context->key(index), binding)).has_value()) {
      ++installed;
    }
  }
  Timer timer;
  const Expected<PathAuthorityGeneration> advanced =
      context->path_authority.Invalidate(path, PathAuthorization::Revoked);
  std::uint64_t completed = 0;
  if (advanced.has_value()) {
    if (context->runtime->OnPathAuthorityChanged(path, advanced.value(), PathAuthorization::Revoked).has_value()) {
      completed = installed;
    }
  }
  report("path-invalidation-" + to_decimal(count), completed, timer.milliseconds());
}

void snapshot_and_digest_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  for (std::uint64_t index = 0; index < count; ++index) {
    (void)context->runtime->PublishRoute(context->request(context->key(index), context->binding()));
  }
  std::uint64_t completed = 0;
  Timer snapshot_timer;
  {
    const Expected<RouteSnapshotSet> snapshot = context->runtime->Snapshot();
    if (snapshot.has_value()) {
      completed = snapshot.value().routes.size();
    }
  }
  report("snapshot-" + to_decimal(count), completed, snapshot_timer.milliseconds());

  std::uint64_t digests = 0;
  Timer digest_timer;
  for (std::uint64_t index = 0; index < count; ++index) {
    const Expected<RouteSnapshot> snapshot = context->runtime->QueryRoute(context->key(index));
    if (snapshot.has_value() && !snapshot.value().digest.is_zero()) {
      ++digests;
    }
  }
  report("digest-" + to_decimal(count), digests, digest_timer.milliseconds());
}

void save_and_load_benchmark(std::uint64_t count) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("routefabric-bench-" + to_decimal(count));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(1);
  state.policy_generation = PolicyGeneration::from_value(1);
  {
    auto context = BenchContext::Create();
    for (std::uint64_t index = 0; index < count; ++index) {
      (void)context->runtime->PublishRoute(context->request(context->key(index), context->binding()));
    }
    const Expected<RouteSnapshotSet> snapshot = context->runtime->Snapshot();
    if (!snapshot.has_value()) {
      std::cout << "save benchmark snapshot failed\n";
      return;
    }
    for (const RouteSnapshot& route : snapshot.value().routes) {
      state.routes.push_back(route.record);
    }
  }
  RouteStore store(base, limits, Durability::Snapshot);
  std::uint64_t saved = 0;
  Timer save_timer;
  if (store.SaveSnapshot(state).has_value()) {
    saved = state.routes.size();
  }
  report("store-save-" + to_decimal(count), saved, save_timer.milliseconds());

  std::uint64_t loaded = 0;
  Timer load_timer;
  const Expected<PersistedState> reloaded = store.Load();
  if (reloaded.has_value()) {
    loaded = reloaded.value().routes.size();
  }
  report("store-load-" + to_decimal(count), loaded, load_timer.milliseconds());
  std::filesystem::remove_all(directory, error);
}

void reconciliation_benchmark(std::uint64_t count) {
  auto context = BenchContext::Create();
  for (std::uint64_t index = 0; index < count; ++index) {
    (void)context->runtime->PublishRoute(context->request(context->key(index), context->binding()));
  }
  context->backend.SetObservedPresence(BackendPresence::Present, "in the fabric");
  std::uint64_t completed = 0;
  Timer timer;
  const Expected<ReconciliationSummary> summary = context->runtime->ReconcileAll();
  if (summary.has_value()) {
    completed = summary.value().checked;
  }
  report("reconciliation-" + to_decimal(count), completed, timer.milliseconds());
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--quick") == 0) {
      quick = true;
    }
  }
  std::cout << "route-fabric-benchmarks " << kVersionString << '\n';
  const std::uint64_t small = quick ? 1000 : 1000;
  const std::uint64_t medium = quick ? 2000 : 10000;
  const std::uint64_t large = quick ? 4000 : 100000;
  publish_many(small);
  publish_many(medium);
  publish_many(large);
  lookup_benchmark(medium);
  replacement_benchmark(medium);
  withdrawal_benchmark(medium);
  path_invalidation_benchmark(medium);
  snapshot_and_digest_benchmark(medium);
  save_and_load_benchmark(small);
  reconciliation_benchmark(medium);
  return 0;
}
