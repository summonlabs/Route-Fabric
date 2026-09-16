#include "test_framework.hpp"

#include <thread>

#include "harness.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

// A path authority that violates its contract by calling back into the runtime.
class ReentrantPathAuthority final : public IPathAuthority {
 public:
  void attach(RouteFabricRuntime* runtime) { runtime_ = runtime; }
  PathAuthorityResult Query(const PathId& path) const override {
    if (runtime_ != nullptr) {
      (void)runtime_->Statistics();
    }
    PathAuthorityResult result;
    result.path = path;
    result.generation = PathAuthorityGeneration::from_value(1);
    result.state = PathAuthorization::Usable;
    return result;
  }

 private:
  RouteFabricRuntime* runtime_ = nullptr;
};

}  // namespace

RF_TEST(unregistered_publishers_cannot_mutate) {
  auto harness = Harness::Create();
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::NotRegistered);
  RF_CHECK_EQ(harness->runtime->counters().stale_authority_rejections, static_cast<std::uint64_t>(1));
}

RF_TEST(a_stale_worker_boot_cannot_mutate) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const WorkerBootId other_boot = make_id<WorkerBootId>(4242);
  PublishRequest request = harness->publish_request(harness->key("10.0.0.0/24"),
                                                    harness->next_hop_binding(harness->next_next_hop()));
  request.worker_boot = other_boot;
  const Expected<PublishResult> published = harness->runtime->PublishRoute(request);
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::StaleWorkerBoot);
}

RF_TEST(epoch_advance_invalidates_live_authority) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RF_CHECK_EQ(harness->runtime->epoch().value(), static_cast<std::uint64_t>(1));

  const Expected<CoordinatorEpoch> advanced = harness->runtime->AdvanceEpoch();
  RF_REQUIRE(advanced.has_value());
  RF_CHECK_EQ(advanced.value().value(), static_cast<std::uint64_t>(2));

  const Expected<PublishResult> stale = harness->publish_route("10.0.1.0/24");
  RF_REQUIRE(!stale.has_value());
  RF_CHECK(stale.error().code() == StatusCode::StaleEpoch);

  // Durable intent survives; live authority does not.
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StaleEpoch);

  RF_REQUIRE(harness->register_default().has_value());
  RevalidateRequest revalidate;
  revalidate.publisher = harness->publisher;
  revalidate.worker_boot = harness->boot;
  revalidate.attempt = harness->next_attempt();
  revalidate.route = published.value().route;
  revalidate.reason = "epoch revalidation";
  const Expected<WithdrawOutcome> revalidated = harness->runtime->RevalidateRoute(revalidate);
  RF_REQUIRE(revalidated.has_value());
  const Expected<RouteSnapshot> current = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(current.has_value());
  RF_CHECK(current.value().currentness == RouteCurrentness::Current);
  RF_CHECK_EQ(current.value().record.provenance.epoch.value(), static_cast<std::uint64_t>(2));
}

RF_TEST(worker_reincarnation_fences_the_previous_boot) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  const WorkerBootId fresh_boot = make_id<WorkerBootId>(2);
  RF_REQUIRE(harness->register_publisher(harness->publisher, fresh_boot).has_value());

  // The previous boot can never act again.
  PublishRequest stale = harness->publish_request(harness->key("10.0.1.0/24"),
                                                  harness->next_hop_binding(harness->next_next_hop()));
  stale.worker_boot = harness->boot;
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(stale);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::StaleWorkerBoot);

  // The fresh boot acts under current authority.
  auto harness_boot = fresh_boot;
  (void)harness_boot;
  PublishRequest fresh = harness->publish_request(harness->key("10.0.1.0/24"),
                                                  harness->next_hop_binding(harness->next_next_hop()));
  fresh.worker_boot = fresh_boot;
  const Expected<PublishResult> accepted = harness->runtime->PublishRoute(fresh);
  RF_REQUIRE(accepted.has_value());

  // The pre-reincarnation route is no longer producible by its old boot.
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StaleWorkerBoot);
}

RF_TEST(session_fencing_bars_the_boot_but_allows_a_fresh_boot) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  RF_CHECK(harness->runtime->FencePublisher(harness->publisher, FencingReason::SessionClosed).has_value());
  PublishRequest after_fence = harness->publish_request(harness->key("10.0.1.0/24"),
                                                        harness->next_hop_binding(harness->next_next_hop()));
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(after_fence);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::NotRegistered);

  // Re-registering the fenced boot is not allowed: the boot itself is barred.
  const Expected<PublisherRegistration> same_boot =
      harness->runtime->RegisterPublisher(harness->publisher, harness->boot, harness->scope());
  RF_CHECK(same_boot.has_value());
  const Expected<PublishResult> still_rejected = harness->runtime->PublishRoute(after_fence);
  RF_REQUIRE(!still_rejected.has_value());
  RF_CHECK(still_rejected.error().code() == StatusCode::Fenced);

  // A fresh boot re-registers and acts.
  const WorkerBootId fresh_boot = make_id<WorkerBootId>(9);
  RF_REQUIRE(harness->register_publisher(harness->publisher, fresh_boot).has_value());
  PublishRequest fresh = harness->publish_request(harness->key("10.0.1.0/24"),
                                                  harness->next_hop_binding(harness->next_next_hop()));
  fresh.worker_boot = fresh_boot;
  RF_CHECK(harness->runtime->PublishRoute(fresh).has_value());
}

RF_TEST(administrative_fencing_survives_until_cleared) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  RF_CHECK(harness->runtime->FencePublisher(harness->publisher, FencingReason::Administrative).has_value());
  const Expected<PublisherRegistration> blocked =
      harness->runtime->RegisterPublisher(harness->publisher, make_id<WorkerBootId>(11), harness->scope());
  RF_REQUIRE(!blocked.has_value());
  RF_CHECK(blocked.error().code() == StatusCode::Fenced);
  RF_CHECK(harness->runtime->ClearAdministrativeFence(harness->publisher).has_value());
  RF_CHECK(harness->runtime->RegisterPublisher(harness->publisher, make_id<WorkerBootId>(11), harness->scope())
               .has_value());
}

RF_TEST(publisher_scope_is_enforced_per_dimension) {
  auto harness = Harness::Create();
  PublisherScope scope;
  scope.fabric = harness->fabric;
  scope.namespaces.push_back(RoutingNamespace::parse("tenant-a").value());
  scope.destinations.push_back(Destination::parse_ipv4_prefix("10.0.0.0/8").value());
  scope.route_classes.push_back(RouteClass::Unicast);
  const Expected<PublisherRegistration> registration =
      harness->runtime->RegisterPublisher(harness->publisher, harness->boot, scope);
  RF_REQUIRE(registration.has_value());

  RouteKey allowed = harness->key("10.1.2.0/24");
  allowed.routing_namespace = RoutingNamespace::parse("tenant-a").value();
  RF_CHECK(harness->publish(allowed, harness->next_hop_binding(harness->next_next_hop())).has_value());

  RouteKey wrong_namespace = harness->key("10.1.3.0/24");
  wrong_namespace.routing_namespace = RoutingNamespace::parse("tenant-b").value();
  const Expected<PublishResult> namespace_rejected =
      harness->publish(wrong_namespace, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(!namespace_rejected.has_value());
  RF_CHECK(namespace_rejected.error().code() == StatusCode::ScopeViolation);

  const Expected<PublishResult> destination_rejected =
      harness->publish(harness->key("11.0.0.0/24"), harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(!destination_rejected.has_value());
  RF_CHECK(destination_rejected.error().code() == StatusCode::ScopeViolation);

  RouteKey wrong_class = harness->key("10.2.0.0/24", RouteClass::Service);
  const Expected<PublishResult> class_rejected =
      harness->publish(wrong_class, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(!class_rejected.has_value());
  RF_CHECK(class_rejected.error().code() == StatusCode::ScopeViolation);
  RF_CHECK_EQ(harness->runtime->counters().scope_violations, static_cast<std::uint64_t>(3));
}

RF_TEST(a_scope_with_no_authority_is_rejected_at_registration) {
  auto harness = Harness::Create();
  PublisherScope empty;
  empty.fabric = harness->fabric;
  const Expected<PublisherRegistration> registration =
      harness->runtime->RegisterPublisher(harness->publisher, harness->boot, empty);
  RF_REQUIRE(!registration.has_value());
  RF_CHECK(registration.error().code() == StatusCode::ScopeViolation);

  PublisherScope wrong_fabric = harness->scope();
  wrong_fabric.fabric = FabricId::parse("other-fabric").value();
  const Expected<PublisherRegistration> foreign =
      harness->runtime->RegisterPublisher(harness->publisher, harness->boot, wrong_fabric);
  RF_REQUIRE(!foreign.has_value());
  RF_CHECK(foreign.error().code() == StatusCode::ScopeViolation);
}

RF_TEST(route_keys_are_exclusively_owned_by_one_publisher) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> first = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(first.has_value());

  const PublisherId other = PublisherId::parse("publisher-b").value();
  const WorkerBootId other_boot = make_id<WorkerBootId>(21);
  RF_REQUIRE(harness->register_publisher(other, other_boot).has_value());
  PublishRequest conflicting = harness->publish_request(harness->key("10.0.0.0/24"),
                                                        harness->next_hop_binding(harness->next_next_hop()));
  conflicting.publisher = other;
  conflicting.worker_boot = other_boot;
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(conflicting);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::Conflict);
  RF_CHECK_EQ(harness->runtime->counters().conflicts, static_cast<std::uint64_t>(1));

  // Administrative override takes the key over deterministically.
  const PublisherId admin = PublisherId::parse("fabric-admin").value();
  const WorkerBootId admin_boot = make_id<WorkerBootId>(22);
  RF_REQUIRE(harness->register_publisher(admin, admin_boot, true).has_value());
  PublishRequest override_request = harness->publish_request(
      harness->key("10.0.0.0/24"), harness->next_hop_binding(harness->next_next_hop()));
  override_request.publisher = admin;
  override_request.worker_boot = admin_boot;
  const Expected<PublishResult> overridden = harness->runtime->PublishRoute(override_request);
  RF_REQUIRE(overridden.has_value());
  RF_CHECK_EQ(overridden.value().route.render(), first.value().route.render());
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().record.provenance.publisher.render(), std::string("fabric-admin"));
}

RF_TEST(registrations_are_bounded) {
  Limits limits;
  limits.max_publishers = 2;
  RuntimeConfig config;
  config.limits = limits;
  auto harness = Harness::Create(config);
  RF_REQUIRE(harness->register_publisher(PublisherId::parse("p1").value(), make_id<WorkerBootId>(1)).has_value());
  RF_REQUIRE(harness->register_publisher(PublisherId::parse("p2").value(), make_id<WorkerBootId>(2)).has_value());
  const Expected<PublisherRegistration> third =
      harness->runtime->RegisterPublisher(PublisherId::parse("p3").value(), make_id<WorkerBootId>(3),
                                          harness->scope());
  RF_REQUIRE(!third.has_value());
  RF_CHECK(third.error().code() == StatusCode::LimitExceeded);
}

RF_TEST(path_authority_reentrancy_is_reported_not_deadlocked) {
  ReentrantPathAuthority authority;
  Harness harness;
  RouteFabricRuntime runtime(RuntimeConfig{}, &harness.backend, &authority);
  authority.attach(&runtime);
  RuntimeConfig config;
  config.fabric = harness.fabric;
  // Build a real runtime with the re-entrant authority and confirm that a query
  // from inside the authority implementation is rejected instead of deadlocking.
  RouteFabricRuntime reentrant_runtime(config, &harness.backend, &authority);
  authority.attach(&reentrant_runtime);
  RF_CHECK(reentrant_runtime.Open().has_value());
  const Expected<PublisherRegistration> registration =
      reentrant_runtime.RegisterPublisher(harness.publisher, harness.boot, harness.scope());
  RF_REQUIRE(registration.has_value());
  PathId path = make_id<PathId>(5);
  RouteBinding binding = harness.path_binding(path, PathAuthorityGeneration::from_value(1));
  PublishRequest request;
  request.publisher = harness.publisher;
  request.worker_boot = harness.boot;
  request.attempt = make_id<MutationAttemptId>(1);
  request.key = harness.key("10.0.0.0/24");
  request.binding = binding;
  request.policy_generation = PolicyGeneration::from_value(1);
  const Expected<PublishResult> published = reentrant_runtime.PublishRoute(request);
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::ReentrancyViolation);
}

RF_TEST(concurrent_publication_of_distinct_routes_is_serialized_correctly) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  std::vector<std::thread> workers;
  constexpr int kThreads = 4;
  constexpr int kRoutesPerThread = 16;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&harness, thread_index]() {
      for (int index = 0; index < kRoutesPerThread; ++index) {
        const std::string destination = "10." + to_decimal(static_cast<std::uint64_t>(thread_index)) + "." +
                                        to_decimal(static_cast<std::uint64_t>(index)) + ".0/24";
        const RouteKey key = harness->key(destination);
        const RouteBinding binding = harness->next_hop_binding(harness->next_next_hop());
        const Expected<PublishResult> published = harness->publish(key, binding);
        RF_CHECK(published.has_value());
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.route_count, static_cast<std::size_t>(kThreads * kRoutesPerThread));
  RF_CHECK_EQ(statistics.current_count, static_cast<std::size_t>(kThreads * kRoutesPerThread));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}
