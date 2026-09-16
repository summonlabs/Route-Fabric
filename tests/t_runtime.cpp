#include "test_framework.hpp"

#include "harness.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

RF_TEST(publish_installs_only_after_a_backend_outcome) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RF_CHECK_EQ(published.value().generation.value(), static_cast<std::uint64_t>(1));
  RF_CHECK_EQ(published.value().authority_generation.value(), static_cast<std::uint64_t>(1));
  RF_CHECK(published.value().lifecycle == RouteLifecycle::Installed);
  RF_CHECK(published.value().applied == AppliedClassification::Applied);
  RF_CHECK(published.value().currentness == RouteCurrentness::Current);

  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.applied.reports_applied());
  RF_CHECK(snapshot.value().record.applied.programmed_generation == published.value().generation);
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(a_committed_desire_is_not_an_installation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  // A deferred dispatch means the backend has not resolved the attempt yet.
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().lifecycle == RouteLifecycle::Installing);
  RF_CHECK(published.value().applied == AppliedClassification::Pending);
  RF_CHECK(published.value().currentness == RouteCurrentness::NotInstalled);

  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE(attempts.size() == 1);
  RF_CHECK(harness->backend.CompleteDeferred(attempts[0], ProgrammingOutcome::Applied, "applied later"));
  const Expected<CompletionDisposition> disposition =
      harness->runtime->ApplyProgrammingCompletion(ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "applied later"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Applied);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::Current);
  RF_CHECK_EQ(snapshot.value().record.applied.detail, std::string("applied later"));
}

RF_TEST(exact_replay_does_not_advance_the_route_generation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const NextHopId next_hop = harness->next_next_hop();
  const RouteBinding binding = harness->next_hop_binding(next_hop);
  const Expected<PublishResult> first = harness->publish(key, binding);
  RF_REQUIRE(first.has_value());
  const Expected<PublishResult> replay = harness->publish(key, binding);
  RF_REQUIRE(replay.has_value());
  RF_CHECK(replay.value().idempotent);
  RF_CHECK_EQ(replay.value().generation.value(), first.value().generation.value());
  RF_CHECK_EQ(replay.value().authority_generation.value(), first.value().authority_generation.value());
  RF_CHECK_EQ(harness->backend.call_count(), static_cast<std::size_t>(1));
  RF_CHECK_EQ(harness->runtime->counters().idempotent_publications, static_cast<std::uint64_t>(1));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(replacement_advances_generation_and_records_supersession) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> first = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(first.has_value());
  const Expected<PublishResult> second = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(second.has_value());
  RF_CHECK(second.value().replaced);
  RF_CHECK(!(second.value().route == RouteId()));
  RF_CHECK_EQ(second.value().route.render(), first.value().route.render());  // the identity is stable
  RF_CHECK_EQ(second.value().generation.value(), static_cast<std::uint64_t>(2));
  RF_CHECK(second.value().generation > first.value().generation);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.supersession.kind == SupersessionKind::Replacement);
  RF_CHECK(snapshot.value().record.supersession.prior_generation == first.value().generation);
  RF_CHECK(snapshot.value().record.supersession.successor_generation == second.value().generation);
  RF_CHECK(!snapshot.value().record.history.empty());
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(expected_generation_is_enforced) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  RF_REQUIRE(harness->publish(key, harness->next_hop_binding(harness->next_next_hop())).has_value());
  PublishRequest request = harness->publish_request(key, harness->next_hop_binding(harness->next_next_hop()));
  request.expected_generation = RouteGeneration::from_value(7);
  const Expected<PublishResult> stale = harness->runtime->PublishRoute(request);
  RF_REQUIRE(!stale.has_value());
  RF_CHECK(stale.error().code() == StatusCode::StaleGeneration);
  PublishRequest correct = harness->publish_request(key, harness->next_hop_binding(harness->next_next_hop()));
  correct.expected_generation = RouteGeneration::from_value(1);
  RF_CHECK(harness->runtime->PublishRoute(correct).has_value());
}

RF_TEST(withdrawal_requested_is_distinct_from_withdrawal_applied) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  WithdrawRequest request;
  request.publisher = harness->publisher;
  request.worker_boot = harness->boot;
  request.attempt = harness->next_attempt();
  request.route = published.value().route;
  request.reason = "withdraw under test";
  const Expected<WithdrawOutcome> withdrawing = harness->runtime->WithdrawRoute(request);
  RF_REQUIRE(withdrawing.has_value());
  RF_CHECK(withdrawing.value().lifecycle == RouteLifecycle::Withdrawing);
  RF_CHECK(withdrawing.value().applied == AppliedClassification::Pending);

  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE(attempts.size() == 1);
  const Expected<CompletionDisposition> disposition =
      harness->runtime->ApplyProgrammingCompletion(ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "removed"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Applied);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Withdrawn);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Withdrawn);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::Withdrawn);
}

RF_TEST(repeated_withdrawal_is_idempotent) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  WithdrawRequest request;
  request.publisher = harness->publisher;
  request.worker_boot = harness->boot;
  request.attempt = harness->next_attempt();
  request.route = published.value().route;
  const Expected<WithdrawOutcome> first = harness->runtime->WithdrawRoute(request);
  RF_REQUIRE(first.has_value());
  request.attempt = harness->next_attempt();
  const Expected<WithdrawOutcome> second = harness->runtime->WithdrawRoute(request);
  RF_REQUIRE(second.has_value());
  RF_CHECK(second.value().already_withdrawn);
  RF_CHECK(second.value().generation == first.value().generation);
  RF_CHECK_EQ(harness->backend.call_count(), static_cast<std::size_t>(2));  // install + first withdrawal only
}

RF_TEST(failed_withdrawal_keeps_the_record_and_requires_revalidation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::RetryableFailure);
  WithdrawRequest request;
  request.publisher = harness->publisher;
  request.worker_boot = harness->boot;
  request.attempt = harness->next_attempt();
  request.route = published.value().route;
  const Expected<WithdrawOutcome> outcome = harness->runtime->WithdrawRoute(request);
  RF_REQUIRE(outcome.has_value());
  RF_CHECK(outcome.value().lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(outcome.value().applied == AppliedClassification::WithdrawFailed);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.applied.reports_applied() == false);
}

RF_TEST(backend_refusals_map_to_failed_and_ambiguity_maps_to_revalidation) {
  const std::pair<ProgrammingOutcome, RouteLifecycle> cases[] = {
      {ProgrammingOutcome::Rejected, RouteLifecycle::Failed},
      {ProgrammingOutcome::NotSupported, RouteLifecycle::Failed},
      {ProgrammingOutcome::PermanentFailure, RouteLifecycle::Failed},
      {ProgrammingOutcome::RetryableFailure, RouteLifecycle::RevalidationRequired},
      {ProgrammingOutcome::Ambiguous, RouteLifecycle::RevalidationRequired},
      {ProgrammingOutcome::BackendUnavailable, RouteLifecycle::RevalidationRequired},
      {ProgrammingOutcome::Idempotent, RouteLifecycle::Installed},
  };
  for (const auto& entry : cases) {
    auto harness = Harness::Create();
    RF_REQUIRE(harness->register_default().has_value());
    harness->backend.EnqueueOutcome(entry.first);
    const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
    RF_REQUIRE(published.has_value());
    RF_CHECK(published.value().lifecycle == entry.second);
    if (entry.first != ProgrammingOutcome::Idempotent) {
      RF_CHECK(published.value().currentness != RouteCurrentness::Current);
    }
    const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
    RF_REQUIRE(snapshot.has_value());
    const Status index_status = harness->runtime->ValidateIndexes();
    RF_CHECK(index_status.has_value());
  }
}

RF_TEST(ambiguity_is_recorded_explicitly) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Ambiguous, false, "acknowledgment lost");
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().applied == AppliedClassification::Ambiguous);
  RF_CHECK(published.value().lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK_EQ(harness->runtime->counters().programming_ambiguous, static_cast<std::uint64_t>(1));
}

RF_TEST(retirement_is_terminal_and_requires_a_new_lineage) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  RetireRequest retire;
  retire.publisher = harness->publisher;
  retire.worker_boot = harness->boot;
  retire.attempt = harness->next_attempt();
  retire.route = published.value().route;
  retire.reason = "decommissioned";
  const Expected<WithdrawOutcome> retired = harness->runtime->RetireRoute(retire);
  RF_REQUIRE(retired.has_value());
  RF_CHECK(retired.value().lifecycle == RouteLifecycle::Retired);

  PublishRequest republish = harness->publish_request(key, harness->next_hop_binding(harness->next_next_hop()));
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(republish);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::Retired);

  // A different publisher cannot take the key over without an administrative
  // override and an explicit new lineage.
  const PublisherId other = PublisherId::parse("publisher-b").value();
  const WorkerBootId other_boot = make_id<WorkerBootId>(77);
  RF_REQUIRE(harness->register_publisher(other, other_boot, true).has_value());
  PublishRequest takeover = harness->publish_request(key, harness->next_hop_binding(harness->next_next_hop()));
  takeover.publisher = other;
  takeover.worker_boot = other_boot;
  const Expected<PublishResult> without_lineage = harness->runtime->PublishRoute(takeover);
  RF_REQUIRE(!without_lineage.has_value());
  RF_CHECK(without_lineage.error().code() == StatusCode::Retired);

  takeover.lineage = make_id<RouteId>(999);
  const Expected<PublishResult> successor = harness->runtime->PublishRoute(takeover);
  RF_REQUIRE(successor.has_value());
  RF_CHECK_EQ(successor.value().route.render(), make_id<RouteId>(999).render());
  RF_CHECK(successor.value().lifecycle == RouteLifecycle::Installed);

  const Expected<RouteSnapshot> old_lineage =
      harness->runtime->QueryRouteById(published.value().route);
  RF_REQUIRE(old_lineage.has_value());
  RF_CHECK(old_lineage.value().record.lifecycle == RouteLifecycle::Superseded);
  RF_CHECK(old_lineage.value().record.supersession.kind == SupersessionKind::LineageTakeover);
  RF_CHECK(old_lineage.value().record.supersession.successor_lineage == make_id<RouteId>(999));
  RF_CHECK(old_lineage.value().currentness == RouteCurrentness::Superseded);
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(revocation_is_durable_generation_bound_and_absolute) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const PublisherId admin = PublisherId::parse("fabric-admin").value();
  const WorkerBootId admin_boot = make_id<WorkerBootId>(500);
  RF_REQUIRE(harness->register_publisher(admin, admin_boot, true).has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  RevokeRequest revoke;
  revoke.publisher = admin;
  revoke.worker_boot = admin_boot;
  revoke.attempt = harness->next_attempt();
  revoke.route = published.value().route;
  revoke.reason = "security revocation";
  const Expected<WithdrawOutcome> revoked = harness->runtime->RevokeRoute(revoke);
  RF_REQUIRE(revoked.has_value());
  RF_CHECK(revoked.value().lifecycle == RouteLifecycle::Retired);

  const Expected<WithdrawOutcome> repeated = harness->runtime->RevokeRoute(revoke);
  RF_REQUIRE(repeated.has_value());
  RF_CHECK(repeated.value().generation == revoked.value().generation);

  PublishRequest recreate = harness->publish_request(harness->key("10.0.0.0/24"),
                                                     harness->next_hop_binding(harness->next_next_hop()));
  recreate.lineage = make_id<RouteId>(1234);
  recreate.publisher = admin;
  recreate.worker_boot = admin_boot;
  recreate.key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> blocked = harness->runtime->PublishRoute(recreate);
  RF_REQUIRE(!blocked.has_value());
  RF_CHECK(blocked.error().code() == StatusCode::Revoked);

  const Expected<RouteSnapshot> snapshot = harness->runtime->QueryRouteById(published.value().route);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::Revoked);
  RF_CHECK(snapshot.value().record.retirement.cause == RetirementCause::Revoked);
  RF_CHECK_EQ(harness->runtime->Statistics().revocation_count, static_cast<std::size_t>(1));
}

RF_TEST(revalidation_reestablishes_authority_only_with_backend_evidence) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Ambiguous);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().lifecycle == RouteLifecycle::RevalidationRequired);

  RevalidateRequest request;
  request.publisher = harness->publisher;
  request.worker_boot = harness->boot;
  request.attempt = harness->next_attempt();
  request.route = published.value().route;
  request.reason = "operator revalidation";
  const Expected<WithdrawOutcome> revalidated = harness->runtime->RevalidateRoute(request);
  RF_REQUIRE(revalidated.has_value());
  RF_CHECK(revalidated.value().lifecycle == RouteLifecycle::Installed);
  RF_CHECK(revalidated.value().generation > published.value().generation);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::Current);
}

RF_TEST(snapshot_diff_and_explanation_are_deterministic) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  RF_REQUIRE(harness->publish_route("10.0.0.0/24").has_value());
  const Expected<RouteSnapshotSet> before = harness->runtime->Snapshot();
  RF_REQUIRE(before.has_value());
  const Expected<PublishResult> replaced =
      harness->publish(harness->key("10.0.0.0/24"), harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(replaced.has_value());
  const Expected<RouteSnapshotSet> after = harness->runtime->Snapshot();
  RF_REQUIRE(after.has_value());

  RF_CHECK(!(before.value().id == after.value().id));
  RF_CHECK(!(before.value().digest == after.value().digest));
  const std::vector<RouteDiffEntry> diff =
      harness->runtime->DiffSnapshots(before.value(), after.value(), replaced.value().route);
  RF_REQUIRE(!diff.empty());
  std::size_t previous_field = 0;
  for (const RouteDiffEntry& entry : diff) {
    RF_CHECK(static_cast<std::size_t>(entry.field) >= previous_field);  // stable field order
    previous_field = static_cast<std::size_t>(entry.field);
  }

  const Expected<RouteExplanation> explanation =
      harness->runtime->ExplainRoute(harness->key("10.0.0.0/24"));
  RF_REQUIRE(explanation.has_value());
  RF_CHECK(explanation.value().authoritative);
  RF_CHECK(!explanation.value().authority_reason.empty());
  RF_CHECK(explanation.value().authority_reason.find("CURRENT") != std::string::npos);
  RF_CHECK(!explanation.value().backend_reason.empty());
  RF_CHECK(explanation.value().render().find("authority-reason") != std::string::npos);
}

RF_TEST(lifecycle_and_publisher_indexes_filter_queries) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  RF_REQUIRE(harness->publish_route("10.0.0.0/24").has_value());
  RF_REQUIRE(harness->publish_route("10.0.1.0/24").has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Rejected);
  RF_REQUIRE(harness->publish_route("10.0.2.0/24").has_value());

  RouteListFilter filter;
  filter.has_lifecycle = true;
  filter.lifecycle = RouteLifecycle::Installed;
  const std::vector<RouteSnapshot> installed = harness->runtime->ListRoutes(filter);
  RF_CHECK_EQ(installed.size(), static_cast<std::size_t>(2));

  filter.lifecycle = RouteLifecycle::Failed;
  const std::vector<RouteSnapshot> failed = harness->runtime->ListRoutes(filter);
  RF_CHECK_EQ(failed.size(), static_cast<std::size_t>(1));

  RouteListFilter by_publisher;
  by_publisher.has_publisher = true;
  by_publisher.publisher = harness->publisher;
  RF_CHECK_EQ(harness->runtime->ListRoutes(by_publisher).size(), static_cast<std::size_t>(3));

  RouteListFilter by_currentness;
  by_currentness.has_currentness = true;
  by_currentness.currentness = RouteCurrentness::Current;
  RF_CHECK_EQ(harness->runtime->ListRoutes(by_currentness).size(), static_cast<std::size_t>(2));

  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.route_count, static_cast<std::size_t>(3));
  RF_CHECK_EQ(statistics.installed_count, static_cast<std::size_t>(2));
  RF_CHECK_EQ(statistics.current_count, static_cast<std::size_t>(2));
  RF_CHECK_EQ(statistics.failed_count, static_cast<std::size_t>(1));
  RF_CHECK(statistics.state_digest.is_zero() == false);
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(batch_publication_reports_each_entry_independently) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  std::vector<PublishRequest> batch;
  batch.push_back(harness->publish_request(harness->key("10.0.0.0/24"), harness->next_hop_binding(harness->next_next_hop())));
  batch.push_back(harness->publish_request(harness->key("10.0.1.0/24"), harness->next_hop_binding(harness->next_next_hop())));
  batch.push_back(harness->publish_request(harness->key("10.0.0.0/24"), harness->next_hop_binding(harness->next_next_hop())));
  const Expected<std::vector<Expected<PublishResult>>> results = harness->runtime->PublishBatch(batch);
  RF_REQUIRE(results.has_value());
  RF_CHECK_EQ(results.value().size(), static_cast<std::size_t>(3));
  RF_CHECK(results.value()[0].has_value());
  RF_CHECK(results.value()[1].has_value());
  RF_CHECK(results.value()[2].has_value());
  // The third entry is an independent replacement of the first key.
  RF_CHECK_EQ(results.value()[2].value().generation.value(), static_cast<std::uint64_t>(2));

  Limits small;
  small.max_batch_mutations = 2;
  RuntimeConfig config;
  config.limits = small;
  auto limited = Harness::Create(config);
  RF_REQUIRE(limited->register_default().has_value());
  const Expected<std::vector<Expected<PublishResult>>> too_large = limited->runtime->PublishBatch(batch);
  RF_REQUIRE(!too_large.has_value());
  RF_CHECK(too_large.error().code() == StatusCode::LimitExceeded);
}

RF_TEST(queries_do_not_mutate_state) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const Digest128 digest_before = harness->runtime->Statistics().state_digest;
  for (int index = 0; index < 16; ++index) {
    const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
    RF_REQUIRE(snapshot.has_value());
    const Expected<RouteExplanation> explanation = harness->runtime->ExplainRoute(harness->key("10.0.0.0/24"));
    RF_REQUIRE(explanation.has_value());
  }
  const Digest128 digest_after = harness->runtime->Statistics().state_digest;
  RF_CHECK(digest_before == digest_after);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().record.generation.value(), static_cast<std::uint64_t>(1));
}
