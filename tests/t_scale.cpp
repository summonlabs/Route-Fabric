#include "test_framework.hpp"

#include "harness.hpp"
#include "routefabric/persistence.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

std::string destination_for(std::uint64_t index) {
  const std::uint64_t second = (index >> 8) & 0xFFu;
  const std::uint64_t third = index & 0xFFu;
  return "10." + to_decimal(second) + "." + to_decimal(third) + ".0/24";
}

}  // namespace

RF_TEST(scale_1000_routes_hold_every_invariant) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  constexpr std::uint64_t kRoutes = 1000;
  for (std::uint64_t index = 0; index < kRoutes; ++index) {
    const Expected<PublishResult> published = harness->publish_route(destination_for(index));
    if (!published) {
      RF_FAIL("publication failed at index " + to_decimal(index) + ": " + published.error().detail());
      return;
    }
  }
  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.route_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK_EQ(statistics.current_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());

  // Repeated replacements.
  for (std::uint64_t index = 0; index < kRoutes; index += 7) {
    const RouteKey key = harness->key(destination_for(index));
    RF_REQUIRE(harness->publish(key, harness->next_hop_binding(harness->next_next_hop())).has_value());
  }
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());

  // Repeated withdrawals.
  for (std::uint64_t index = 0; index < kRoutes; index += 11) {
    const RouteKey key = harness->key(destination_for(index));
    const Expected<RouteSnapshot> snapshot = harness->query(key);
    RF_REQUIRE(snapshot.has_value());
    WithdrawRequest withdraw;
    withdraw.publisher = harness->publisher;
    withdraw.worker_boot = harness->boot;
    withdraw.attempt = harness->next_attempt();
    withdraw.route = snapshot.value().record.id;
    RF_REQUIRE(harness->runtime->WithdrawRoute(withdraw).has_value());
  }
  RF_CHECK_EQ(harness->runtime->Statistics().withdrawn_count, static_cast<std::size_t>((kRoutes + 10) / 11));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());

  const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().routes.size(), static_cast<std::size_t>(kRoutes));

  // Exact lookup must remain available at scale.
  for (std::uint64_t index = 0; index < kRoutes; index += 97) {
    const Expected<RouteSnapshot> found = harness->query(harness->key(destination_for(index)));
    RF_REQUIRE(found.has_value());
    RF_CHECK_EQ(found.value().record.key.destination.render(), destination_for(index));
  }
}

RF_TEST(scale_10000_routes_hold_every_invariant) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  constexpr std::uint64_t kRoutes = 10000;
  for (std::uint64_t index = 0; index < kRoutes; ++index) {
    const Expected<PublishResult> published = harness->publish_route(destination_for(index));
    if (!published) {
      RF_FAIL("publication failed at index " + to_decimal(index) + ": " + published.error().detail());
      return;
    }
  }
  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.route_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK_EQ(statistics.current_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
  const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().routes.size(), static_cast<std::size_t>(kRoutes));
  RF_CHECK(!snapshot.value().digest.is_zero());
}

RF_TEST(mass_path_invalidation_touches_only_dependents) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  constexpr std::uint64_t kRoutes = 512;
  const PathId shared_path = make_id<PathId>(1);
  const PathAuthorityGeneration generation = PathAuthorityGeneration::from_value(1);
  RF_REQUIRE(harness->path_authority.SetPath(shared_path, generation, PathAuthorization::Usable).has_value());
  for (std::uint64_t index = 0; index < kRoutes; ++index) {
    const RouteBinding binding = (index % 2 == 0) ? harness->path_binding(shared_path, generation)
                                                  : harness->next_hop_binding(harness->next_next_hop());
    RF_REQUIRE(harness->publish(harness->key(destination_for(index)), binding).has_value());
  }
  const Digest128 before = harness->runtime->Statistics().state_digest;
  const Expected<PathAuthorityGeneration> invalidated =
      harness->path_authority.Invalidate(shared_path, PathAuthorization::Revoked);
  RF_REQUIRE(invalidated.has_value());
  RF_REQUIRE(harness->runtime->OnPathAuthorityChanged(shared_path, invalidated.value(), PathAuthorization::Revoked)
                 .has_value());
  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.revalidation_required_count, static_cast<std::size_t>(kRoutes / 2));
  RF_CHECK_EQ(statistics.installed_count, static_cast<std::size_t>(kRoutes / 2));
  RF_CHECK(!(before == statistics.state_digest));
  RF_CHECK_EQ(statistics.path_dependency_count, static_cast<std::size_t>(1));
  RF_CHECK(harness->runtime->ValidateIndexes().has_value());
}

RF_TEST(mass_epoch_invalidation_keeps_intent_and_drops_authority) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  constexpr std::uint64_t kRoutes = 512;
  for (std::uint64_t index = 0; index < kRoutes; ++index) {
    RF_REQUIRE(harness->publish_route(destination_for(index)).has_value());
  }
  RF_REQUIRE(harness->runtime->AdvanceEpoch().has_value());
  const RouteStatistics statistics = harness->runtime->Statistics();
  RF_CHECK_EQ(statistics.route_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK_EQ(statistics.installed_count, static_cast<std::size_t>(kRoutes));
  RF_CHECK_EQ(statistics.current_count, static_cast<std::size_t>(0));
  RouteListFilter filter;
  filter.has_currentness = true;
  filter.currentness = RouteCurrentness::StaleEpoch;
  RF_CHECK_EQ(harness->runtime->ListRoutes(filter).size(), static_cast<std::size_t>(kRoutes));
}

RF_TEST(save_and_load_at_scale) {
  const std::filesystem::path directory = rftest::make_temp_directory("scale-store");
  const std::filesystem::path base = directory / "fabric";
  constexpr std::uint64_t kRoutes = 1000;
  Limits limits;
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(2);
  state.policy_generation = PolicyGeneration::from_value(1);
  {
    auto harness = Harness::Create();
    RF_REQUIRE(harness->register_default().has_value());
    for (std::uint64_t index = 0; index < kRoutes; ++index) {
      RF_REQUIRE(harness->publish_route(destination_for(index)).has_value());
    }
    const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
    RF_REQUIRE(snapshot.has_value());
    for (const RouteSnapshot& route : snapshot.value().routes) {
      state.routes.push_back(route.record);
    }
  }
  RouteStore store(base, limits, Durability::Snapshot);
  RF_REQUIRE(store.SaveSnapshot(state).has_value());
  Expected<PersistedState> loaded = store.Load();
  RF_REQUIRE(loaded.has_value());
  RF_CHECK_EQ(loaded.value().routes.size(), static_cast<std::size_t>(kRoutes));
  RF_CHECK(loaded.value().digest() == state.digest());
  rftest::remove_directory(directory);
}

RF_TEST(durable_mutations_at_scale_survive_a_restart) {
  const std::filesystem::path directory = rftest::make_temp_directory("scale-journal");
  const std::filesystem::path base = directory / "fabric";
  constexpr std::uint64_t kRoutes = 256;
  auto harness = Harness::Create();
  RuntimeConfig config;
  config.fabric = harness->fabric;
  config.durability = Durability::Journal;
  config.store_path = base;
  {
    auto durable = Harness::Create(config);
    RF_REQUIRE(durable->register_default().has_value());
    for (std::uint64_t index = 0; index < kRoutes; ++index) {
      RF_REQUIRE(durable->publish_route(destination_for(index)).has_value());
    }
    // Compaction keeps the journal bounded without losing acknowledged state.
    RF_REQUIRE(durable->runtime->Compact().has_value());
    RF_REQUIRE(durable->publish_route(destination_for(kRoutes)).has_value());
  }
  auto restarted = Harness::Create(config, false);
  RF_REQUIRE(restarted->runtime->Open().has_value());
  RF_CHECK_EQ(restarted->runtime->Statistics().route_count, static_cast<std::size_t>(kRoutes + 1));
  RF_CHECK(restarted->runtime->ValidateIndexes().has_value());
  rftest::remove_directory(directory);
}
