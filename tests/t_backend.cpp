#include "test_framework.hpp"

#include "harness.hpp"
#include "routefabric/real_backend.hpp"

using namespace routefabric;
using rftest::Harness;

RF_TEST(synthetic_backend_is_scripted_deterministically) {
  SyntheticProgrammingBackend backend(BackendId::parse("synthetic-test").value());
  RouteProgrammingRequest request;
  request.attempt = rftest::make_id<ProgrammingAttemptId>(1);
  request.programming_generation = ProgrammingGeneration::from_value(1);
  request.route = rftest::make_id<RouteId>(1);
  request.key = Harness().key("10.0.0.0/24");
  request.binding = Harness().next_hop_binding(rftest::make_id<NextHopId>(1));
  request.desired_generation = RouteGeneration::from_value(1);
  request.authority_generation = RouteAuthorityGeneration::from_value(1);
  request.epoch = CoordinatorEpoch::from_value(1);
  request.publisher = PublisherId::parse("publisher-a").value();
  request.worker_boot = rftest::make_id<WorkerBootId>(1);

  backend.EnqueueOutcome(ProgrammingOutcome::Rejected, false, "first");
  backend.EnqueueOutcome(ProgrammingOutcome::Ambiguous, false, "second");
  backend.SetDefaultOutcome(ProgrammingOutcome::Applied);

  const ProgrammingDispatch first = backend.InstallRoute(request);
  RF_CHECK(!first.deferred);
  RF_CHECK(first.immediate.outcome == ProgrammingOutcome::Rejected);
  RF_CHECK_EQ(first.immediate.detail, std::string("first"));
  const ProgrammingDispatch second = backend.InstallRoute(request);
  RF_CHECK(second.immediate.outcome == ProgrammingOutcome::Ambiguous);
  const ProgrammingDispatch third = backend.InstallRoute(request);
  RF_CHECK(third.immediate.outcome == ProgrammingOutcome::Applied);
  RF_CHECK_EQ(backend.call_count(), static_cast<std::size_t>(3));
  RF_CHECK_EQ(backend.RecordedCalls().size(), static_cast<std::size_t>(3));
  RF_CHECK_EQ(backend.RecordedCalls()[0].attempt.render(), request.attempt.render());
}

RF_TEST(synthetic_backend_supports_deferred_completion) {
  SyntheticProgrammingBackend backend(BackendId::parse("synthetic-test").value());
  RouteProgrammingRequest request;
  request.attempt = rftest::make_id<ProgrammingAttemptId>(7);
  request.programming_generation = ProgrammingGeneration::from_value(1);
  request.route = rftest::make_id<RouteId>(1);
  request.key = Harness().key("10.0.0.0/24");
  request.binding = Harness().next_hop_binding(rftest::make_id<NextHopId>(1));
  request.desired_generation = RouteGeneration::from_value(1);
  request.authority_generation = RouteAuthorityGeneration::from_value(1);
  request.epoch = CoordinatorEpoch::from_value(1);
  request.publisher = PublisherId::parse("publisher-a").value();
  request.worker_boot = rftest::make_id<WorkerBootId>(1);
  backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const ProgrammingDispatch dispatch = backend.InstallRoute(request);
  RF_CHECK(dispatch.deferred);
  RF_CHECK(backend.HasDeferred(request.attempt));
  RF_CHECK_EQ(backend.DeferredAttempts().size(), static_cast<std::size_t>(1));
  RF_CHECK(backend.CompleteDeferred(request.attempt, ProgrammingOutcome::Ambiguous, "resolved late"));
  RF_CHECK(!backend.CompleteDeferred(rftest::make_id<ProgrammingAttemptId>(8), ProgrammingOutcome::Applied));
}

RF_TEST(synthetic_backend_reports_divergent_observed_state) {
  SyntheticProgrammingBackend backend(BackendId::parse("synthetic-test").value());
  backend.SetObservedPresence(BackendPresence::Present, "present in fabric");
  backend.SetObservedGeneration(RouteGeneration::from_value(4));
  const BackendQueryResult query = backend.QueryRoute(Harness().key("10.0.0.0/24"));
  RF_CHECK(query.presence == BackendPresence::Present);
  RF_CHECK(query.observed_generation == RouteGeneration::from_value(4));
  backend.SetAvailable(false);
  const BackendQueryResult unavailable = backend.QueryRoute(Harness().key("10.0.0.0/24"));
  RF_CHECK(unavailable.presence == BackendPresence::Unavailable);
}

RF_TEST(real_host_route_adapter_is_read_only_and_real) {
  RF_CHECK(WindowsRouteTableBackend::is_supported());
  WindowsRouteTableBackend backend;
  const BackendCapabilities capabilities = backend.capabilities();
  RF_CHECK(capabilities.query);
  RF_CHECK(capabilities.read_only);
  RF_CHECK(!capabilities.install);
  RF_CHECK(!capabilities.replace);
  RF_CHECK(!capabilities.withdraw);
  RF_CHECK_EQ(backend.id().render(), std::string("windows-host-routing-table"));

  RouteProgrammingRequest request;
  request.attempt = rftest::make_id<ProgrammingAttemptId>(1);
  request.programming_generation = ProgrammingGeneration::from_value(1);
  request.route = rftest::make_id<RouteId>(1);
  request.key = Harness().key("10.0.0.0/24");
  request.binding = Harness().next_hop_binding(rftest::make_id<NextHopId>(1));
  request.desired_generation = RouteGeneration::from_value(1);
  request.authority_generation = RouteAuthorityGeneration::from_value(1);
  request.epoch = CoordinatorEpoch::from_value(1);
  request.publisher = PublisherId::parse("publisher-a").value();
  request.worker_boot = rftest::make_id<WorkerBootId>(1);
  RF_CHECK(backend.InstallRoute(request).immediate.outcome == ProgrammingOutcome::NotSupported);
  RF_CHECK(backend.ReplaceRoute(request).immediate.outcome == ProgrammingOutcome::NotSupported);
  RF_CHECK(backend.WithdrawRoute(request).immediate.outcome == ProgrammingOutcome::NotSupported);
}

RF_TEST(real_host_route_evidence_is_genuine) {
  WindowsRouteTableBackend backend;
  const std::vector<HostRouteEntry> entries = backend.EnumerateHostRoutes();
  // A functioning host always has at least one route (for example the loopback
  // prefix). This asserts real evidence, not a synthetic list.
  RF_CHECK(!entries.empty());
  if (entries.empty()) {
    RF_CHECK_EQ(backend.last_error(), std::string());
    return;
  }
  bool saw_loopback = false;
  for (const HostRouteEntry& entry : entries) {
    RF_CHECK(entry.destination.is_valid());
    RF_CHECK(!entry.render().empty());
    if (entry.destination.kind() == DestinationKind::Ipv4Prefix &&
        entry.destination.render() == "127.0.0.0/8") {
      saw_loopback = true;
    }
  }
  RF_CHECK(saw_loopback);
  const RouteKey loopback = Harness().key("127.0.0.0/8");
  const BackendQueryResult query = backend.QueryRoute(loopback);
  RF_CHECK(query.presence == BackendPresence::Present);
  const RouteKey absent = Harness().key("203.0.113.0/24");
  RF_CHECK(backend.QueryRoute(absent).presence == BackendPresence::Absent);
}

RF_TEST(a_read_only_backend_makes_the_runtime_fail_rather_than_claim_success) {
  WindowsRouteTableBackend backend;
  SyntheticPathAuthority path_authority;
  RuntimeConfig config;
  config.fabric = FabricId::parse("fabric").value();
  RouteFabricRuntime runtime(config, &backend, &path_authority);
  RF_REQUIRE(runtime.Open().has_value());
  PublisherScope scope;
  scope.fabric = config.fabric;
  scope.wildcard_namespaces = true;
  scope.wildcard_destinations = true;
  scope.wildcard_route_classes = true;
  const PublisherId publisher = PublisherId::parse("real-backend-publisher").value();
  const WorkerBootId boot = rftest::make_id<WorkerBootId>(1);
  RF_REQUIRE(runtime.RegisterPublisher(publisher, boot, scope).has_value());

  const std::vector<HostRouteEntry> before = backend.EnumerateHostRoutes();
  PublishRequest request;
  request.publisher = publisher;
  request.worker_boot = boot;
  request.attempt = rftest::make_id<MutationAttemptId>(1);
  request.key = Harness().key("198.51.100.0/24");
  request.binding = Harness().next_hop_binding(rftest::make_id<NextHopId>(1));
  request.policy_generation = PolicyGeneration::from_value(1);
  const Expected<PublishResult> published = runtime.PublishRoute(request);
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().applied == AppliedClassification::NotSupported);
  RF_CHECK(published.value().lifecycle == RouteLifecycle::Failed);
  RF_CHECK(published.value().currentness == RouteCurrentness::Failed);

  // The host routing table must be untouched by a failed programming attempt.
  const std::vector<HostRouteEntry> after = backend.EnumerateHostRoutes();
  RF_CHECK_EQ(before.size(), after.size());
  const BackendQueryResult query = backend.QueryRoute(Harness().key("198.51.100.0/24"));
  RF_CHECK(query.presence == BackendPresence::Absent);
}

RF_TEST(reconciliation_records_evidence_without_replacing_authority) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  harness->backend.SetObservedGeneration(RouteGeneration::from_value(99));
  const Expected<ObservationClass> diverged = harness->runtime->ReconcileRoute(published.value().route);
  RF_REQUIRE(diverged.has_value());
  RF_CHECK(diverged.value() == ObservationClass::Diverged);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.observation.classification == ObservationClass::Diverged);
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StaleBackendObservation);

  harness->backend.SetObservedGeneration(RouteGeneration());
  harness->backend.SetObservedPresence(BackendPresence::Absent, "removed out of band");
  const Expected<ObservationClass> missing = harness->runtime->ReconcileRoute(published.value().route);
  RF_REQUIRE(missing.has_value());
  RF_CHECK(missing.value() == ObservationClass::Missing);
  const Expected<RouteSnapshot> missing_snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(missing_snapshot.has_value());
  RF_CHECK(missing_snapshot.value().currentness == RouteCurrentness::DesiredAppliedMismatch);
  RF_CHECK(missing_snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK_EQ(harness->runtime->counters().reconciliations, static_cast<std::uint64_t>(2));

  harness->backend.SetObservedPresence(BackendPresence::Present);
  const Expected<ReconciliationSummary> summary = harness->runtime->ReconcileAll();
  RF_REQUIRE(summary.has_value());
  RF_CHECK_EQ(summary.value().checked, static_cast<std::size_t>(1));
  RF_CHECK_EQ(summary.value().matched, static_cast<std::size_t>(1));
}
