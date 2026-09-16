#include "test_framework.hpp"

#include <atomic>
#include <thread>

#include "harness.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

RF_TEST(a_late_install_completion_cannot_overwrite_a_newer_generation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> first = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(first.has_value());
  RF_CHECK(first.value().lifecycle == RouteLifecycle::Installing);
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));

  // Generation N+1 supersedes the in-flight attempt.
  const Expected<PublishResult> second = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE_OK(second);
  RF_CHECK(second.value().lifecycle == RouteLifecycle::Installed);
  RF_CHECK(second.value().generation > first.value().generation);

  // The stale completion arrives late and must be classified, not applied.
  const Expected<CompletionDisposition> disposition =
      harness->runtime->ApplyProgrammingCompletion(ProgrammingCompletion{attempts[0], ProgrammingOutcome::Rejected, "stale"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Stale);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.generation == second.value().generation);
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Applied);
  RF_CHECK_EQ(harness->runtime->counters().stale_completions_rejected, static_cast<std::uint64_t>(1));
}

RF_TEST(a_duplicate_completion_is_rejected) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const RouteProgrammingRequest request = harness->backend.RecordedCalls()[0];
  const Expected<CompletionDisposition> duplicate =
      harness->runtime->ApplyProgrammingCompletion(ProgrammingCompletion{request.attempt, ProgrammingOutcome::Applied, "again"});
  RF_REQUIRE(duplicate.has_value());
  RF_CHECK(duplicate.value() == CompletionDisposition::Stale);
  const Expected<CompletionDisposition> unknown = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{make_id<ProgrammingAttemptId>(9999), ProgrammingOutcome::Applied, "unknown"});
  RF_REQUIRE(unknown.has_value());
  RF_CHECK(unknown.value() == CompletionDisposition::UnknownAttempt);
}

RF_TEST(a_late_withdrawal_completion_cannot_undo_a_replacement) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());

  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  WithdrawRequest withdraw;
  withdraw.publisher = harness->publisher;
  withdraw.worker_boot = harness->boot;
  withdraw.attempt = harness->next_attempt();
  withdraw.route = published.value().route;
  RF_REQUIRE(harness->runtime->WithdrawRoute(withdraw).has_value());
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));

  const Expected<PublishResult> replacement = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE_OK(replacement);
  RF_CHECK(replacement.value().lifecycle == RouteLifecycle::Installed);

  const Expected<CompletionDisposition> disposition = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "withdrawn late"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Stale);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Applied);
}

RF_TEST(path_invalidation_wins_over_an_in_flight_install) {
  auto harness = Harness::Create();
  const PathId path = make_id<PathId>(11);
  const PathAuthorityGeneration generation = PathAuthorityGeneration::from_value(1);
  RF_REQUIRE(harness->path_authority.SetPath(path, generation, PathAuthorization::Usable).has_value());
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish(key, harness->path_binding(path, generation));
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().lifecycle == RouteLifecycle::Installing);
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));

  const Expected<PathAuthorityGeneration> invalidated =
      harness->path_authority.Invalidate(path, PathAuthorization::Revoked);
  RF_REQUIRE(invalidated.has_value());
  RF_REQUIRE(harness->runtime->OnPathAuthorityChanged(path, invalidated.value(), PathAuthorization::Revoked).has_value());

  const Expected<CompletionDisposition> disposition = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "installed before the revocation"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Stale);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(!is_current(snapshot.value().currentness));
}

RF_TEST(an_epoch_advance_wins_over_an_in_flight_install) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));
  RF_REQUIRE(harness->runtime->AdvanceEpoch().has_value());
  const Expected<CompletionDisposition> disposition = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "completed after the epoch change"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Stale);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installing);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StaleEpoch);
}

RF_TEST(a_fenced_publisher_never_produces_a_current_route) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));
  RF_REQUIRE(harness->runtime->FencePublisher(harness->publisher, FencingReason::WorkerDeath).has_value());
  const Expected<CompletionDisposition> disposition = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "applied while the worker was dying"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Applied);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  // The backend evidence is recorded, but the route is not current authority.
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Applied);
  RF_CHECK(!is_current(snapshot.value().currentness));
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StalePublisher);
}

RF_TEST(retirement_wins_over_a_delayed_completion) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));

  RetireRequest retire;
  retire.publisher = harness->publisher;
  retire.worker_boot = harness->boot;
  retire.attempt = harness->next_attempt();
  retire.route = published.value().route;
  RF_REQUIRE(harness->runtime->RetireRoute(retire).has_value());

  const Expected<CompletionDisposition> disposition = harness->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "applied just before retirement"});
  RF_REQUIRE(disposition.has_value());
  RF_CHECK(disposition.value() == CompletionDisposition::Stale);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Retired);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::Retired);
  RF_CHECK(!is_current(snapshot.value().currentness));
}

RF_TEST(concurrent_expected_generation_publication_has_exactly_one_winner) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  RF_REQUIRE(harness->publish(key, harness->next_hop_binding(harness->next_next_hop())).has_value());

  std::atomic<int> successes{0};
  std::atomic<int> stale{0};
  const auto worker = [&harness, &key, &successes, &stale]() {
    PublishRequest request = harness->publish_request(key, harness->next_hop_binding(make_id<NextHopId>(777)));
    request.expected_generation = RouteGeneration::from_value(1);
    const Expected<PublishResult> published = harness->runtime->PublishRoute(request);
    if (published) {
      successes.fetch_add(1);
    } else if (published.error().code() == StatusCode::StaleGeneration) {
      stale.fetch_add(1);
    }
  };
  std::thread first(worker);
  std::thread second(worker);
  first.join();
  second.join();
  RF_CHECK_EQ(successes.load(), 1);
  RF_CHECK_EQ(stale.load(), 1);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().record.generation.value(), static_cast<std::uint64_t>(2));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(concurrent_withdrawals_dispatch_exactly_one_backend_operation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  const std::size_t calls_before = harness->backend.call_count();

  std::atomic<int> dispatched{0};
  std::atomic<int> idempotent{0};
  std::atomic<int> in_flight{0};
  std::atomic<std::uint64_t> attempt_sequence{0};
  const auto worker = [&harness, &published, &dispatched, &idempotent, &in_flight, &attempt_sequence]() {
    WithdrawRequest request;
    request.publisher = harness->publisher;
    request.worker_boot = harness->boot;
    request.attempt = make_id<MutationAttemptId>(attempt_sequence.fetch_add(1) + 100);
    request.route = published.value().route;
    const Expected<WithdrawOutcome> outcome = harness->runtime->WithdrawRoute(request);
    if (!outcome) {
      return;
    }
    if (outcome.value().already_withdrawn) {
      idempotent.fetch_add(1);
    } else if (outcome.value().withdrawal_in_flight) {
      in_flight.fetch_add(1);
    } else {
      dispatched.fetch_add(1);
    }
  };
  std::thread first(worker);
  std::thread second(worker);
  first.join();
  second.join();
  RF_CHECK_EQ(dispatched.load(), 1);
  // The second caller observes either a completed withdrawal (idempotent) or one
  // that is still in flight; both are explicit and neither dispatches backend work.
  RF_CHECK_EQ(dispatched.load() + idempotent.load() + in_flight.load(), 2);
  RF_CHECK_EQ(harness->backend.call_count(), calls_before + 1);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Withdrawn);
}

RF_TEST(snapshots_taken_during_mutation_are_consistent) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  std::atomic<bool> stop{false};
  std::atomic<int> snapshots{0};
  std::thread observer([&harness, &stop, &snapshots]() {
    while (!stop.load()) {
      const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
      if (snapshot) {
        for (const RouteSnapshot& route : snapshot.value().routes) {
          // Every snapshot must carry a self-consistent record.
          RF_CHECK(route.record.generation.is_valid());
          RF_CHECK(route.record.binding.is_valid());
          RF_CHECK(!route.id.is_zero());
        }
        snapshots.fetch_add(1);
      }
    }
  });
  for (int index = 0; index < 64; ++index) {
    const std::string destination = "10.0." + to_decimal(static_cast<std::uint64_t>(index)) + ".0/24";
    RF_CHECK(harness->publish_route(destination).has_value());
  }
  stop.store(true);
  observer.join();
  RF_CHECK(snapshots.load() > 0);
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
  RF_CHECK_EQ(harness->runtime->Statistics().route_count, static_cast<std::size_t>(64));
}

RF_TEST(a_lifecycle_mutation_racing_a_completion_resolves_deterministically) {
  // The completion is delivered from a second thread while the main thread
  // supersedes the route. Whichever order occurs, the route must end up
  // consistent and the stale outcome must never become applied truth.
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const Expected<PublishResult> published = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(published.has_value());
  const std::vector<ProgrammingAttemptId> attempts = harness->backend.DeferredAttempts();
  RF_REQUIRE_EQ(attempts.size(), static_cast<std::size_t>(1));

  std::atomic<bool> start{false};
  std::thread completer([&harness, &attempts, &start]() {
    while (!start.load()) {
      std::this_thread::yield();
    }
    (void)harness->runtime->ApplyProgrammingCompletion(
        ProgrammingCompletion{attempts[0], ProgrammingOutcome::Rejected, "late rejection"});
  });
  start.store(true);
  const Expected<PublishResult> replacement = harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
  completer.join();
  RF_REQUIRE_OK(replacement);
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.generation == replacement.value().generation);
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Applied);
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}
