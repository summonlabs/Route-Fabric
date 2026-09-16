#include "test_framework.hpp"

#include <map>
#include <set>

#include "harness.hpp"
#include "routefabric/persistence.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

// Deterministic schedule driver. Every run is fully determined by its seed and
// the seed is reported on failure.
struct Schedule {
  explicit Schedule(std::uint64_t schedule_seed) : state(schedule_seed) {}

  std::uint64_t next(std::uint64_t bound) {
    state = mix64(state + 0x9E3779B97F4A7C15ull);
    return bound == 0 ? 0 : state % bound;
  }

  std::uint64_t state;
};

struct Outcome {
  std::map<RouteId, RouteGeneration> generations;
  std::map<RouteId, RouteLifecycle> lifecycles;
  Digest128 digest;
  std::size_t operations = 0;
};

// Runs one deterministic schedule against a fresh runtime and reports the final
// state, verifying the core invariants after every step.
bool run_schedule(std::uint64_t seed, Outcome& outcome, std::string& failure) {
  auto harness = Harness::Create();
  if (!harness->register_default().has_value()) {
    failure = "registration failed";
    return false;
  }
  Schedule schedule(seed);
  constexpr std::size_t kDestinations = 8;
  std::vector<RouteKey> keys;
  for (std::size_t index = 0; index < kDestinations; ++index) {
    keys.push_back(harness->key("10.0." + to_decimal(index) + ".0/24"));
  }
  std::vector<RouteId> known;

  const auto verify = [&](const char* step) {
    const Status indexes = harness->runtime->ValidateIndexes();
    if (!indexes) {
      failure = std::string(step) + ": " + indexes.error().detail();
      return false;
    }
    const RouteStatistics statistics = harness->runtime->Statistics();
    if (statistics.route_count != harness->runtime->ListRoutes(RouteListFilter{}).size()) {
      failure = std::string(step) + ": route count disagrees with the listing";
      return false;
    }
    for (const auto& pair : outcome.generations) {
      const Expected<RouteSnapshot> snapshot = harness->runtime->QueryRouteById(pair.first);
      if (!snapshot) {
        failure = std::string(step) + ": a known route disappeared";
        return false;
      }
      if (snapshot.value().record.generation < pair.second) {
        failure = std::string(step) + ": route generation decreased";
        return false;
      }
      if (snapshot.value().record.applied.programmed_generation.is_valid() &&
          snapshot.value().record.applied.programmed_generation > snapshot.value().record.generation) {
        failure = std::string(step) + ": applied generation exceeds the desired generation";
        return false;
      }
      if (snapshot.value().record.lifecycle == RouteLifecycle::Retired &&
          is_current(snapshot.value().currentness)) {
        failure = std::string(step) + ": a retired route became current";
        return false;
      }
      if (snapshot.value().record.lifecycle == RouteLifecycle::Withdrawn &&
          snapshot.value().record.applied.reports_applied()) {
        failure = std::string(step) + ": a withdrawn route claims to be installed";
        return false;
      }
    }
    return true;
  };

  constexpr std::size_t kOperations = 120;
  for (std::size_t step = 0; step < kOperations; ++step) {
    const std::uint64_t choice = schedule.next(10);
    const RouteKey& key = keys[schedule.next(kDestinations)];
    const Expected<RouteSnapshot> existing = harness->runtime->QueryRoute(key);
    ++outcome.operations;
    if (choice < 5) {
      const Expected<PublishResult> published =
          harness->publish(key, harness->next_hop_binding(harness->next_next_hop()));
      if (published) {
        outcome.generations[published.value().route] = published.value().generation;
        outcome.lifecycles[published.value().route] = published.value().lifecycle;
        if (std::find(known.begin(), known.end(), published.value().route) == known.end()) {
          known.push_back(published.value().route);
        }
      } else if (published.error().code() != StatusCode::Retired &&
                 published.error().code() != StatusCode::Revoked &&
                 published.error().code() != StatusCode::Conflict) {
        failure = "unexpected publication failure: " + published.error().detail();
        return false;
      }
    } else if (choice < 7 && existing) {
      WithdrawRequest request;
      request.publisher = harness->publisher;
      request.worker_boot = harness->boot;
      request.attempt = harness->next_attempt();
      request.route = existing.value().record.id;
      const Expected<WithdrawOutcome> withdrawn = harness->runtime->WithdrawRoute(request);
      if (withdrawn) {
        outcome.generations[existing.value().record.id] = withdrawn.value().generation;
        outcome.lifecycles[existing.value().record.id] = withdrawn.value().lifecycle;
      } else if (withdrawn.error().code() != StatusCode::Retired &&
                 withdrawn.error().code() != StatusCode::Revoked) {
        failure = "unexpected withdrawal failure: " + withdrawn.error().detail();
        return false;
      }
    } else if (choice < 8 && existing) {
      RevalidateRequest request;
      request.publisher = harness->publisher;
      request.worker_boot = harness->boot;
      request.attempt = harness->next_attempt();
      request.route = existing.value().record.id;
      const Expected<WithdrawOutcome> revalidated = harness->runtime->RevalidateRoute(request);
      if (revalidated) {
        outcome.generations[existing.value().record.id] = revalidated.value().generation;
      }
    } else if (choice < 9 && existing) {
      RetireRequest request;
      request.publisher = harness->publisher;
      request.worker_boot = harness->boot;
      request.attempt = harness->next_attempt();
      request.route = existing.value().record.id;
      const Expected<WithdrawOutcome> retired = harness->runtime->RetireRoute(request);
      if (retired) {
        outcome.generations[existing.value().record.id] = retired.value().generation;
        outcome.lifecycles[existing.value().record.id] = retired.value().lifecycle;
      }
    } else if (existing) {
      const Expected<ObservationClass> classification =
          harness->runtime->ReconcileRoute(existing.value().record.id);
      if (!classification) {
        failure = "unexpected reconciliation failure";
        return false;
      }
    }

    // Property: at most one authoritative route per route key.
    const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
    if (!snapshot) {
      failure = "snapshot failed";
      return false;
    }
    std::set<RouteKey> seen_keys;
    for (const RouteSnapshot& route : snapshot.value().routes) {
      if (!seen_keys.insert(route.record.key).second) {
        failure = "two records claim the same route key";
        return false;
      }
    }
    if (!verify("schedule step")) {
      return false;
    }
  }
  outcome.digest = harness->runtime->Statistics().state_digest;
  return true;
}

}  // namespace

RF_TEST(seeded_schedules_hold_every_invariant) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    Outcome outcome;
    std::string failure;
    if (!run_schedule(seed, outcome, failure)) {
      RF_FAIL("seed " + to_decimal(seed) + ": " + failure);
      return;
    }
    RF_CHECK(outcome.operations > 0);
    RF_CHECK(!outcome.digest.is_zero());
  }
}

RF_TEST(the_same_schedule_produces_the_same_state_digest) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    Outcome first;
    Outcome second;
    std::string failure;
    RF_REQUIRE(run_schedule(seed, first, failure));
    if (!run_schedule(seed, second, failure)) {
      RF_FAIL("seed " + to_decimal(seed) + ": " + failure);
      return;
    }
    RF_CHECK(first.digest == second.digest);
  }
}

RF_TEST(equivalent_state_reached_in_different_orders_hashes_identically) {
  auto forward = Harness::Create();
  auto backward = Harness::Create();
  RF_REQUIRE(forward->register_default().has_value());
  RF_REQUIRE(backward->register_default().has_value());
  const std::vector<std::string> destinations = {"10.0.0.0/24", "10.0.1.0/24", "10.0.2.0/24", "10.0.3.0/24"};
  // The binding is a pure function of the destination so that the two orders
  // produce genuinely equivalent state.
  const auto binding_for = [](std::size_t index) {
    return Harness().next_hop_binding(make_id<NextHopId>(index + 1));
  };
  // The publication identity is derived from the destination as well, so the two
  // orders describe exactly the same logical publications.
  const auto request_for = [&destinations, &binding_for](Harness& harness, std::size_t index) {
    PublishRequest request = harness.publish_request(harness.key(destinations[index]), binding_for(index));
    request.attempt = make_id<MutationAttemptId>(index + 1);
    return request;
  };
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    RF_REQUIRE(forward->runtime->PublishRoute(request_for(*forward, index)).has_value());
  }
  for (std::size_t index = destinations.size(); index-- > 0;) {
    RF_REQUIRE(backward->runtime->PublishRoute(request_for(*backward, index)).has_value());
  }
  RF_CHECK(forward->runtime->Statistics().state_digest == backward->runtime->Statistics().state_digest);
  const Expected<RouteSnapshotSet> left = forward->runtime->Snapshot();
  const Expected<RouteSnapshotSet> right = backward->runtime->Snapshot();
  RF_REQUIRE(left.has_value());
  RF_REQUIRE(right.has_value());
  RF_CHECK(left.value().digest == right.value().digest);
  RF_CHECK(left.value().id == right.value().id);
}

RF_TEST(persisted_state_round_trips_through_the_encoder_for_every_seed) {
  Limits limits;
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    Outcome outcome;
    std::string failure;
    auto harness = Harness::Create();
    RF_REQUIRE(harness->register_default().has_value());
    Schedule schedule(seed);
    for (int index = 0; index < 24; ++index) {
      const std::string destination = "10.1." + to_decimal(schedule.next(32)) + ".0/24";
      (void)harness->publish_route(destination);
    }
    const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
    RF_REQUIRE(snapshot.has_value());
    PersistedState state;
    state.epoch = harness->runtime->epoch();
    state.policy_generation = PolicyGeneration::from_value(1);
    for (const RouteSnapshot& route : snapshot.value().routes) {
      state.routes.push_back(route.record);
    }
    const std::vector<std::uint8_t> image = RouteStore::EncodeSnapshot(state, limits);
    RF_REQUIRE(!image.empty());
    PersistedState decoded;
    std::string why;
    RF_REQUIRE(RouteStore::DecodeSnapshot(image, limits, decoded, why));
    RF_CHECK_EQ(decoded.routes.size(), state.routes.size());
    RF_CHECK(decoded.digest() == state.digest());
    for (std::size_t index = 0; index < state.routes.size(); ++index) {
      RF_CHECK(semantic_digest(decoded.routes[index]) == semantic_digest(state.routes[index]));
    }
  }
}

RF_TEST(repeated_identical_replays_leave_the_generation_untouched) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  const RouteBinding binding = harness->next_hop_binding(harness->next_next_hop());
  const Expected<PublishResult> first = harness->publish(key, binding);
  RF_REQUIRE(first.has_value());
  for (int index = 0; index < 32; ++index) {
    const Expected<PublishResult> replay = harness->publish(key, binding);
    RF_REQUIRE(replay.has_value());
    RF_CHECK(replay.value().idempotent);
    RF_CHECK(replay.value().generation == first.value().generation);
  }
  RF_CHECK_EQ(harness->runtime->counters().publications, static_cast<std::uint64_t>(1));
  RF_CHECK_EQ(harness->runtime->counters().replacements, static_cast<std::uint64_t>(0));
  RF_CHECK_EQ(harness->backend.call_count(), static_cast<std::size_t>(1));
}
