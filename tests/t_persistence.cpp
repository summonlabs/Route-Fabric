#include "test_framework.hpp"

#include <fstream>

#include "harness.hpp"
#include "routefabric/persistence.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

PersistedState make_state() {
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(3);
  state.policy_generation = PolicyGeneration::from_value(2);

  RouteRecord record;
  record.id = make_id<RouteId>(1);
  record.key = Harness().key("10.0.0.0/24");
  record.generation = RouteGeneration::from_value(4);
  record.authority_generation = RouteAuthorityGeneration::from_value(2);
  record.invalidation_watermark = RouteAuthorityGeneration::from_value(2);
  record.programming_generation = ProgrammingGeneration::from_value(3);
  record.lifecycle = RouteLifecycle::Installed;
  record.binding = Harness().next_hop_binding(make_id<NextHopId>(9));
  record.applied.classification = AppliedClassification::Applied;
  record.applied.attempt = make_id<ProgrammingAttemptId>(5);
  record.applied.programming_generation = ProgrammingGeneration::from_value(3);
  record.applied.programmed_generation = RouteGeneration::from_value(4);
  record.applied.programmed_authority_generation = RouteAuthorityGeneration::from_value(2);
  record.applied.backend = BackendId::parse("synthetic-test").value();
  record.applied.detail = "applied";
  record.observation.classification = ObservationClass::Matched;
  record.observation.attempt = make_id<ProgrammingAttemptId>(5);
  record.observation.observed_generation = RouteGeneration::from_value(4);
  record.observation.backend = BackendId::parse("synthetic-test").value();
  record.provenance.publisher = PublisherId::parse("publisher-a").value();
  record.provenance.worker_boot = make_id<WorkerBootId>(1);
  record.provenance.epoch = CoordinatorEpoch::from_value(3);
  record.provenance.source_class = ProvenanceSourceClass::Publisher;
  record.provenance.source_generation = RouteGeneration::from_value(4);
  record.provenance.mutation_attempt = make_id<MutationAttemptId>(2);
  record.provenance.policy_generation = PolicyGeneration::from_value(2);
  record.provenance.path_authority_generation = PathAuthorityGeneration::from_value(1);

  LineageEntry entry;
  entry.generation = RouteGeneration::from_value(4);
  entry.event = RouteEvent::BeginInstall;
  entry.lifecycle = RouteLifecycle::Installing;
  entry.binding_kind = BindingKind::NextHop;
  entry.publisher = record.provenance.publisher;
  entry.epoch = record.provenance.epoch;
  record.history.push_back(entry);

  state.routes.push_back(record);

  RevocationRecord revocation;
  revocation.route = make_id<RouteId>(2);
  revocation.key = Harness().key("10.0.1.0/24");
  revocation.generation = RouteGeneration::from_value(2);
  revocation.epoch = CoordinatorEpoch::from_value(3);
  revocation.attempt = make_id<MutationAttemptId>(3);
  revocation.reason = "revoked";
  state.revocations.push_back(revocation);

  ProgrammingAttemptRecord attempt;
  attempt.attempt = make_id<ProgrammingAttemptId>(5);
  attempt.route = record.id;
  attempt.desired_generation = RouteGeneration::from_value(4);
  attempt.programming_generation = ProgrammingGeneration::from_value(3);
  attempt.operation = ProgrammingOperation::Install;
  attempt.epoch = CoordinatorEpoch::from_value(3);
  attempt.resolved = true;
  state.attempts.push_back(attempt);
  return state;
}

std::vector<std::uint8_t> snapshot_image(const PersistedState& state, const Limits& limits) {
  return RouteStore::EncodeSnapshot(state, limits);
}

bool decode_snapshot(const std::vector<std::uint8_t>& image, const Limits& limits, PersistedState& state,
                     std::string& why) {
  return RouteStore::DecodeSnapshot(image, limits, state, why);
}

}  // namespace

RF_TEST(persisted_state_round_trips_through_the_encoder) {
  Limits limits;
  const PersistedState state = make_state();
  const std::vector<std::uint8_t> image = snapshot_image(state, limits);
  RF_REQUIRE(!image.empty());
  PersistedState decoded;
  std::string why;
  RF_REQUIRE(decode_snapshot(image, limits, decoded, why));
  RF_CHECK_EQ(decoded.routes.size(), static_cast<std::size_t>(1));
  RF_CHECK_EQ(decoded.revocations.size(), static_cast<std::size_t>(1));
  RF_CHECK_EQ(decoded.attempts.size(), static_cast<std::size_t>(1));
  RF_CHECK(decoded.epoch == state.epoch);
  RF_CHECK(decoded.routes[0] == state.routes[0]);
  RF_CHECK(decoded.digest() == state.digest());
  RF_CHECK_EQ(decoded.routes[0].applied.detail, std::string("applied"));
}

RF_TEST(persistence_rejects_every_truncation_point) {
  Limits limits;
  const std::vector<std::uint8_t> image = snapshot_image(make_state(), limits);
  RF_REQUIRE(image.size() > 32);
  for (std::size_t length = 0; length < image.size(); ++length) {
    const std::vector<std::uint8_t> truncated(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(length));
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(truncated, limits, decoded, why));
  }
}

RF_TEST(persistence_rejects_structural_corruption) {
  Limits limits;
  const std::vector<std::uint8_t> image = snapshot_image(make_state(), limits);

  {  // empty file
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(std::vector<std::uint8_t>(), limits, decoded, why));
  }
  {  // wrong magic
    std::vector<std::uint8_t> corrupted = image;
    corrupted[0] ^= 0xFFu;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
  {  // unsupported version
    std::vector<std::uint8_t> corrupted = image;
    write_le32(corrupted.data() + 8, 99);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
  {  // corrupted integrity trailer
    std::vector<std::uint8_t> corrupted = image;
    corrupted[corrupted.size() - 1] ^= 0x01u;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
  {  // trailing bytes
    std::vector<std::uint8_t> corrupted = image;
    corrupted.push_back(0);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
  {  // flipped payload byte (integrity failure)
    std::vector<std::uint8_t> corrupted = image;
    corrupted[kPersistFrameHeaderBytes + 4] ^= 0x40u;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
  {  // oversized payload declaration
    std::vector<std::uint8_t> corrupted = image;
    write_le32(corrupted.data() + 12, 0xFFFFFFFFu);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(corrupted, limits, decoded, why));
  }
}

RF_TEST(persistence_rejects_impossible_semantics_with_valid_integrity) {
  Limits limits;

  // Applies a mutation to the encoded payload of the assembled state, recomputes
  // the integrity trailer, and requires the semantic validation to reject it.
  const auto rebuild = [&limits](const PersistedState& state) {
    return snapshot_image(state, limits);
  };

  {  // duplicate RouteId
    PersistedState state = make_state();
    state.routes.push_back(state.routes[0]);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // duplicate route key with distinct identity is not possible through the
     // encoder, so craft the payload directly.
    PersistedState state = make_state();
    RouteRecord duplicate = state.routes[0];
    duplicate.id = make_id<RouteId>(77);
    state.routes.push_back(duplicate);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // applied generation newer than the desired generation
    PersistedState state = make_state();
    state.routes[0].applied.programmed_generation = RouteGeneration::from_value(9);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // impossible route generation (zero)
    PersistedState state = make_state();
    state.routes[0].generation = RouteGeneration();
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // retired route without a retirement record
    PersistedState state = make_state();
    state.routes[0].lifecycle = RouteLifecycle::Retired;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // superseded route without a supersession record
    PersistedState state = make_state();
    state.routes[0].lifecycle = RouteLifecycle::Superseded;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // malformed desired binding
    PersistedState state = make_state();
    state.routes[0].binding = RouteBinding();
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // invalid destination inside the route key
    PersistedState state = make_state();
    state.routes[0].key.destination = Destination();
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // installed route without an applied backend outcome
    PersistedState state = make_state();
    state.routes[0].applied.classification = AppliedClassification::Pending;
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(rebuild(state), limits, decoded, why));
  }
  {  // state digest mismatch
    PersistedState state = make_state();
    ByteWriter payload;
    payload.u64(state.epoch.value());
    payload.u64(state.policy_generation.value());
    payload.u32(0);  // no routes
    payload.u32(0);
    payload.u32(0);
    payload.raw(Digest128::from_u64_pair(1, 2).bytes());
    const std::vector<std::uint8_t> image = encode_persistence_frame(kPersistenceMagic, payload.buffer());
    PersistedState decoded;
    std::string why;
    RF_CHECK(!decode_snapshot(image, limits, decoded, why));
  }
}

RF_TEST(persistence_rejects_oversized_counts) {
  Limits limits;
  ByteWriter payload;
  payload.u64(1);
  payload.u64(1);
  payload.u32(0xFFFFFFFFu);  // impossible route count
  const std::vector<std::uint8_t> image = encode_persistence_frame(kPersistenceMagic, payload.buffer());
  PersistedState decoded;
  std::string why;
  RF_CHECK(!decode_snapshot(image, limits, decoded, why));
}

RF_TEST(persistence_rejects_malformed_enum_values) {
  Limits limits;
  const PersistedState state = make_state();
  std::vector<std::uint8_t> image = snapshot_image(state, limits);
  // Locate the lifecycle byte by rebuilding the payload with a poisoned value.
  PersistedState poisoned = state;
  poisoned.routes[0].lifecycle = static_cast<RouteLifecycle>(200);
  const std::vector<std::uint8_t> poisoned_image = snapshot_image(poisoned, limits);
  PersistedState decoded;
  std::string why;
  RF_CHECK(!decode_snapshot(poisoned_image, limits, decoded, why));
  (void)image;
}

RF_TEST(snapshot_store_survives_a_restart) {
  const std::filesystem::path directory = rftest::make_temp_directory("store-snapshot");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  {
    RouteStore store(base, limits, Durability::Snapshot);
    RF_REQUIRE(store.SaveSnapshot(make_state()).has_value());
    RF_CHECK(store.exists());
  }
  RouteStore reopened(base, limits, Durability::Snapshot);
  Expected<PersistedState> loaded = reopened.Load();
  RF_REQUIRE(loaded.has_value());
  RF_CHECK_EQ(loaded.value().routes.size(), static_cast<std::size_t>(1));
  RF_CHECK(loaded.value().epoch == CoordinatorEpoch::from_value(3));
  rftest::remove_directory(directory);
}

RF_TEST(journal_entries_are_flushed_and_replayed) {
  const std::filesystem::path directory = rftest::make_temp_directory("store-journal");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  PersistedState initial;
  initial.epoch = CoordinatorEpoch::from_value(1);
  initial.policy_generation = PolicyGeneration::from_value(1);
  {
    RouteStore store(base, limits, Durability::Journal);
    RF_REQUIRE(store.SaveSnapshot(initial).has_value());
    JournalEntry entry;
    entry.epoch = CoordinatorEpoch::from_value(1);
    entry.policy_generation = PolicyGeneration::from_value(1);
    entry.upserts.push_back(make_state().routes[0]);
    RF_REQUIRE(store.AppendJournal(entry).has_value());
    RF_CHECK_EQ(store.journal_entry_count(), static_cast<std::size_t>(1));
  }
  RouteStore reopened(base, limits, Durability::Journal);
  Expected<PersistedState> loaded = reopened.Load();
  RF_REQUIRE(loaded.has_value());
  RF_CHECK_EQ(loaded.value().routes.size(), static_cast<std::size_t>(1));
  RF_CHECK_EQ(reopened.journal_entry_count(), static_cast<std::size_t>(1));
  rftest::remove_directory(directory);
}

RF_TEST(a_torn_journal_tail_is_discarded_and_truncated) {
  const std::filesystem::path directory = rftest::make_temp_directory("store-torn");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  {
    RouteStore store(base, limits, Durability::Journal);
    PersistedState initial;
    initial.epoch = CoordinatorEpoch::from_value(1);
    initial.policy_generation = PolicyGeneration::from_value(1);
    RF_REQUIRE(store.SaveSnapshot(initial).has_value());
    JournalEntry entry;
    entry.epoch = CoordinatorEpoch::from_value(1);
    entry.policy_generation = PolicyGeneration::from_value(1);
    entry.upserts.push_back(make_state().routes[0]);
    RF_REQUIRE(store.AppendJournal(entry).has_value());
  }
  // Simulate a torn write: append half of another entry.
  {
    JournalEntry entry;
    entry.epoch = CoordinatorEpoch::from_value(1);
    entry.policy_generation = PolicyGeneration::from_value(1);
    entry.upserts.push_back(make_state().routes[0]);
    const std::vector<std::uint8_t> image = RouteStore::EncodeJournalEntry(entry, limits);
    std::ofstream out(base.string() + ".journal", std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size() / 2));
  }
  RouteStore reopened(base, limits, Durability::Journal);
  Expected<PersistedState> loaded = reopened.Load();
  RF_REQUIRE(loaded.has_value());
  RF_CHECK_EQ(loaded.value().routes.size(), static_cast<std::size_t>(1));
  RF_CHECK_EQ(reopened.journal_entry_count(), static_cast<std::size_t>(1));

  // The torn tail is gone: a new append must be readable afterwards.
  JournalEntry next;
  next.epoch = CoordinatorEpoch::from_value(1);
  next.policy_generation = PolicyGeneration::from_value(1);
  RouteRecord second = make_state().routes[0];
  second.id = make_id<RouteId>(9);
  second.key = Harness().key("10.0.3.0/24");
  next.upserts.push_back(second);
  RF_REQUIRE(reopened.AppendJournal(next).has_value());
  RouteStore third(base, limits, Durability::Journal);
  Expected<PersistedState> reloaded = third.Load();
  RF_REQUIRE(reloaded.has_value());
  RF_CHECK_EQ(reloaded.value().routes.size(), static_cast<std::size_t>(2));
  rftest::remove_directory(directory);
}

RF_TEST(a_corrupt_journal_body_is_not_replayed) {
  const std::filesystem::path directory = rftest::make_temp_directory("store-corrupt-journal");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  {
    RouteStore store(base, limits, Durability::Journal);
    PersistedState initial;
    initial.epoch = CoordinatorEpoch::from_value(1);
    initial.policy_generation = PolicyGeneration::from_value(1);
    RF_REQUIRE(store.SaveSnapshot(initial).has_value());
    JournalEntry entry;
    entry.epoch = CoordinatorEpoch::from_value(1);
    entry.policy_generation = PolicyGeneration::from_value(1);
    entry.upserts.push_back(make_state().routes[0]);
    RF_REQUIRE(store.AppendJournal(entry).has_value());
  }
  const std::string journal_path = base.string() + ".journal";
  {
    std::vector<std::uint8_t> bytes;
    std::string read_why;
    RF_REQUIRE(read_file_bytes(journal_path, bytes, read_why));
    bytes[kPersistFrameHeaderBytes + 2] ^= 0x80u;
    std::ofstream out(journal_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  RouteStore reopened(base, limits, Durability::Journal);
  Expected<PersistedState> loaded = reopened.Load();
  RF_REQUIRE(loaded.has_value());
  RF_CHECK_EQ(loaded.value().routes.size(), static_cast<std::size_t>(0));
  rftest::remove_directory(directory);
}

RF_TEST(runtime_persists_intent_before_acknowledging_and_recovers_conservatively) {
  const std::filesystem::path directory = rftest::make_temp_directory("runtime-restart");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;

  {
    auto harness = Harness::Create();
    RuntimeConfig config;
    config.fabric = harness->fabric;
    config.durability = Durability::Journal;
    config.store_path = base;
    auto durable = Harness::Create(config);
    RF_REQUIRE(durable->register_default().has_value());
    const Expected<PublishResult> published = durable->publish_route("10.0.0.0/24");
    RF_REQUIRE(published.has_value());
    RF_CHECK(published.value().currentness == RouteCurrentness::Current);
  }

  // A fresh runtime over the same store: the durable intent survives and live
  // publisher authority does not.
  auto harness = Harness::Create();
  RuntimeConfig config;
  config.fabric = harness->fabric;
  config.durability = Durability::Journal;
  config.store_path = base;
  auto restarted = Harness::Create(config, false);
  RF_REQUIRE(restarted->runtime->Open().has_value());
  const Expected<RouteSnapshot> snapshot = restarted->query(restarted->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StalePublisher);
  RF_CHECK(!is_current(snapshot.value().currentness));

  // Re-registering and revalidating is the only path back to current authority.
  RF_REQUIRE(restarted->register_default().has_value());
  RevalidateRequest revalidate;
  revalidate.publisher = restarted->publisher;
  revalidate.worker_boot = restarted->boot;
  revalidate.attempt = restarted->next_attempt();
  revalidate.route = snapshot.value().record.id;
  revalidate.reason = "restart revalidation";
  const Expected<WithdrawOutcome> revalidated = restarted->runtime->RevalidateRoute(revalidate);
  RF_REQUIRE(revalidated.has_value());
  const Expected<RouteSnapshot> current = restarted->query(restarted->key("10.0.0.0/24"));
  RF_REQUIRE(current.has_value());
  RF_CHECK(current.value().currentness == RouteCurrentness::Current);
  RF_CHECK(restarted->runtime->ValidateIndexes().has_value());
  rftest::remove_directory(directory);
}

RF_TEST(an_unresolved_attempt_is_recovered_as_revalidation_required) {
  const std::filesystem::path directory = rftest::make_temp_directory("runtime-ambiguous-restart");
  const std::filesystem::path base = directory / "fabric";

  {
    auto harness = Harness::Create();
    RuntimeConfig config;
    config.fabric = harness->fabric;
    config.durability = Durability::Journal;
    config.store_path = base;
    auto durable = Harness::Create(config);
    RF_REQUIRE(durable->register_default().has_value());
    durable->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);  // deferred, never completed
    const Expected<PublishResult> published = durable->publish_route("10.0.0.0/24");
    RF_REQUIRE(published.has_value());
    RF_CHECK(published.value().lifecycle == RouteLifecycle::Installing);
  }

  auto harness = Harness::Create();
  RuntimeConfig config;
  config.fabric = harness->fabric;
  config.durability = Durability::Journal;
  config.store_path = base;
  auto restarted = Harness::Create(config, false);
  RF_REQUIRE(restarted->runtime->Open().has_value());
  const Expected<RouteSnapshot> snapshot = restarted->query(restarted->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Ambiguous);
  RF_CHECK(!is_current(snapshot.value().currentness));
  rftest::remove_directory(directory);
}

RF_TEST(store_inspection_helpers_behave) {
  const std::filesystem::path directory = rftest::make_temp_directory("store-helpers");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  RouteStore store(base, limits, Durability::Snapshot);
  RF_CHECK(!store.exists());
  PersistedState state = make_state();
  RF_REQUIRE(store.SaveSnapshot(state).has_value());
  RF_CHECK(store.exists());
  RF_REQUIRE(store.Remove().has_value());
  RF_CHECK(!store.exists());
  rftest::remove_directory(directory);
}
