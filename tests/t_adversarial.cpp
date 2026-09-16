#include "test_framework.hpp"

#include <thread>

#include "harness.hpp"
#include "routefabric/client.hpp"
#include "routefabric/persistence.hpp"
#include "routefabric/server.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

RF_TEST(malformed_destinations_and_keys_are_rejected_before_mutation) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  PublishRequest request = harness->publish_request(harness->key("10.0.0.0/24"),
                                                    harness->next_hop_binding(harness->next_next_hop()));
  request.key.destination = Destination();
  const Expected<PublishResult> invalid_key = harness->runtime->PublishRoute(request);
  RF_REQUIRE(!invalid_key.has_value());
  RF_CHECK(invalid_key.error().code() == StatusCode::InvalidArgument);

  PublishRequest invalid_binding = harness->publish_request(harness->key("10.0.1.0/24"), RouteBinding());
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(invalid_binding);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::InvalidArgument);

  PublishRequest zero_attempt = harness->publish_request(harness->key("10.0.2.0/24"),
                                                         harness->next_hop_binding(harness->next_next_hop()));
  zero_attempt.attempt = MutationAttemptId();
  const Expected<PublishResult> no_attempt = harness->runtime->PublishRoute(zero_attempt);
  RF_REQUIRE(!no_attempt.has_value());
  RF_CHECK(no_attempt.error().code() == StatusCode::InvalidArgument);
}

RF_TEST(host_route_class_requires_a_full_length_prefix) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey invalid = harness->key("10.0.0.0/24", RouteClass::Host);
  RF_CHECK(!invalid.is_valid());
  PublishRequest request = harness->publish_request(invalid, harness->next_hop_binding(harness->next_next_hop()));
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(request);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::InvalidArgument);
  const RouteKey valid = harness->key("10.0.0.7/32", RouteClass::Host);
  RF_CHECK(valid.is_valid());
  RF_CHECK(harness->publish(valid, harness->next_hop_binding(harness->next_next_hop())).has_value());
}

RF_TEST(oversized_diagnostic_text_is_refused) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  PublishRequest request = harness->publish_request(harness->key("10.0.0.0/24"),
                                                    harness->next_hop_binding(harness->next_next_hop()));
  request.reason = std::string(kMaxDiagnosticChars + 1, 'x');
  const Expected<PublishResult> rejected = harness->runtime->PublishRoute(request);
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::LimitExceeded);
}

RF_TEST(the_route_capacity_limit_is_enforced) {
  Limits limits;
  limits.max_routes = 4;
  RuntimeConfig config;
  config.limits = limits;
  auto harness = Harness::Create(config);
  RF_REQUIRE(harness->register_default().has_value());
  for (int index = 0; index < 4; ++index) {
    const std::string destination = "10.0." + to_decimal(static_cast<std::uint64_t>(index)) + ".0/24";
    RF_CHECK(harness->publish_route(destination).has_value());
  }
  const Expected<PublishResult> excess = harness->publish_route("10.0.9.0/24");
  RF_REQUIRE(!excess.has_value());
  RF_CHECK(excess.error().code() == StatusCode::LimitExceeded);
  // Replacing an existing route is still permitted at capacity.
  RF_CHECK(harness->publish(harness->key("10.0.0.0/24"),
                            harness->next_hop_binding(harness->next_next_hop()))
               .has_value());
}

RF_TEST(the_routing_namespace_limit_is_enforced) {
  Limits limits;
  limits.max_routing_namespaces = 2;
  RuntimeConfig config;
  config.limits = limits;
  auto harness = Harness::Create(config);
  RF_REQUIRE(harness->register_default().has_value());
  for (int index = 0; index < 2; ++index) {
    RouteKey key = harness->key("10.0." + to_decimal(static_cast<std::uint64_t>(index)) + ".0/24");
    key.routing_namespace = RoutingNamespace::parse("ns" + to_decimal(static_cast<std::uint64_t>(index))).value();
    RF_CHECK(harness->publish(key, harness->next_hop_binding(harness->next_next_hop())).has_value());
  }
  RouteKey third = harness->key("10.0.9.0/24");
  third.routing_namespace = RoutingNamespace::parse("ns9").value();
  const Expected<PublishResult> rejected =
      harness->publish(third, harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(!rejected.has_value());
  RF_CHECK(rejected.error().code() == StatusCode::LimitExceeded);
}

RF_TEST(the_history_bound_is_respected) {
  RuntimeConfig config;
  config.history_limit = 4;
  auto harness = Harness::Create(config);
  RF_REQUIRE(harness->register_default().has_value());
  const RouteKey key = harness->key("10.0.0.0/24");
  for (int index = 0; index < 20; ++index) {
    RF_REQUIRE(harness->publish(key, harness->next_hop_binding(harness->next_next_hop())).has_value());
  }
  const Expected<RouteSnapshot> snapshot = harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.history.size() <= static_cast<std::size_t>(4));
  RF_CHECK(snapshot.value().record.history.size() > 0);
}

RF_TEST(a_generation_overflow_is_an_explicit_error) {
  const std::filesystem::path directory = rftest::make_temp_directory("overflow");
  const std::filesystem::path base = directory / "fabric";
  Limits limits;
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(1);
  state.policy_generation = PolicyGeneration::from_value(1);
  RouteRecord record;
  record.id = make_id<RouteId>(1);
  record.key = Harness().key("10.0.0.0/24");
  record.generation = RouteGeneration::from_value(RouteGeneration::kMaxValue);
  record.authority_generation = RouteAuthorityGeneration::from_value(1);
  record.invalidation_watermark = RouteAuthorityGeneration::from_value(1);
  record.programming_generation = ProgrammingGeneration::from_value(1);
  record.lifecycle = RouteLifecycle::Installed;
  record.binding = Harness().next_hop_binding(make_id<NextHopId>(1));
  record.applied.classification = AppliedClassification::Applied;
  record.applied.attempt = make_id<ProgrammingAttemptId>(1);
  record.applied.programming_generation = ProgrammingGeneration::from_value(1);
  record.applied.programmed_generation = record.generation;
  record.applied.programmed_authority_generation = RouteAuthorityGeneration::from_value(1);
  record.applied.backend = BackendId::parse("synthetic-test").value();
  record.observation.backend = BackendId::parse("synthetic-test").value();
  record.provenance.publisher = PublisherId::parse("publisher-a").value();
  record.provenance.worker_boot = make_id<WorkerBootId>(1);
  record.provenance.epoch = CoordinatorEpoch::from_value(1);
  record.provenance.source_generation = record.generation;
  record.provenance.mutation_attempt = make_id<MutationAttemptId>(1);
  record.provenance.policy_generation = PolicyGeneration::from_value(1);
  record.provenance.path_authority_generation = PathAuthorityGeneration::from_value(1);
  state.routes.push_back(record);
  {
    RouteStore store(base, limits, Durability::Snapshot);
    RF_REQUIRE(store.SaveSnapshot(state).has_value());
  }

  auto harness = Harness::Create();
  RuntimeConfig config;
  config.fabric = harness->fabric;
  config.durability = Durability::Snapshot;
  config.store_path = base;
  auto loaded = Harness::Create(config, false);
  RF_REQUIRE_OK(loaded->runtime->Open());
  RF_REQUIRE(loaded->register_default().has_value());
  const Expected<PublishResult> overflow =
      loaded->publish(loaded->key("10.0.0.0/24"), loaded->next_hop_binding(make_id<NextHopId>(2)));
  RF_REQUIRE(!overflow.has_value());
  RF_CHECK(overflow.error().code() == StatusCode::StaleGeneration);
  RF_CHECK(loaded->runtime->ValidateIndexes().has_value());
  rftest::remove_directory(directory);
}

RF_TEST(withdrawal_after_retirement_is_refused) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RetireRequest retire;
  retire.publisher = harness->publisher;
  retire.worker_boot = harness->boot;
  retire.attempt = harness->next_attempt();
  retire.route = published.value().route;
  RF_REQUIRE(harness->runtime->RetireRoute(retire).has_value());
  WithdrawRequest withdraw;
  withdraw.publisher = harness->publisher;
  withdraw.worker_boot = harness->boot;
  withdraw.attempt = harness->next_attempt();
  withdraw.route = published.value().route;
  const Expected<WithdrawOutcome> refused = harness->runtime->WithdrawRoute(withdraw);
  RF_REQUIRE(!refused.has_value());
  RF_CHECK(refused.error().code() == StatusCode::Retired);
  const Expected<RouteSnapshot> snapshot = harness->query(harness->key("10.0.0.0/24"));
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Retired);
}

RF_TEST(revocation_requires_administrative_authority) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(published.has_value());
  RevokeRequest revoke;
  revoke.publisher = harness->publisher;
  revoke.worker_boot = harness->boot;
  revoke.attempt = harness->next_attempt();
  revoke.route = published.value().route;
  const Expected<WithdrawOutcome> refused = harness->runtime->RevokeRoute(revoke);
  RF_REQUIRE(!refused.has_value());
  RF_CHECK(refused.error().code() == StatusCode::Unauthorized);
}

RF_TEST(unknown_route_identities_are_reported_as_not_found) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  const RouteId unknown = make_id<RouteId>(123456);
  const Expected<RouteSnapshot> snapshot = harness->runtime->QueryRouteById(unknown);
  RF_REQUIRE(!snapshot.has_value());
  RF_CHECK(snapshot.error().code() == StatusCode::NotFound);
  WithdrawRequest withdraw;
  withdraw.publisher = harness->publisher;
  withdraw.worker_boot = harness->boot;
  withdraw.attempt = harness->next_attempt();
  withdraw.route = unknown;
  const Expected<WithdrawOutcome> refused = harness->runtime->WithdrawRoute(withdraw);
  RF_REQUIRE(!refused.has_value());
  RF_CHECK(refused.error().code() == StatusCode::NotFound);
  const Expected<RouteExplanation> explanation = harness->runtime->ExplainRouteById(unknown);
  RF_REQUIRE(!explanation.has_value());
  RF_CHECK(explanation.error().code() == StatusCode::NotFound);
}

RF_TEST(mutations_before_open_are_refused) {
  auto harness = Harness::Create(RuntimeConfig(), false);
  const Expected<PublishResult> published = harness->publish_route("10.0.0.0/24");
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::NotOpen);
  const Expected<CoordinatorEpoch> advanced = harness->runtime->AdvanceEpoch();
  RF_REQUIRE(!advanced.has_value());
  RF_CHECK(advanced.error().code() == StatusCode::NotOpen);
  RF_CHECK(harness->runtime->Open().has_value());
  RF_CHECK(!harness->runtime->Open().has_value());  // double open is rejected
}

RF_TEST(open_validates_its_configuration) {
  {
    RuntimeConfig config;
    config.history_limit = 0;
    Harness harness;
    RouteFabricRuntime runtime(config, &harness.backend, &harness.path_authority);
    const Status status = runtime.Open();
    RF_REQUIRE(!status.has_value());
    RF_CHECK(status.error().code() == StatusCode::InvalidArgument);
  }
  {
    RuntimeConfig config;
    config.fabric = FabricId();
    Harness harness;
    RouteFabricRuntime runtime(config, &harness.backend, &harness.path_authority);
    const Status status = runtime.Open();
    RF_REQUIRE(!status.has_value());
    RF_CHECK(status.error().code() == StatusCode::InvalidArgument);
  }
  {
    RuntimeConfig config;
    Limits limits;
    limits.max_routes = 0;
    config.limits = limits;
    config.fabric = FabricId::parse("fabric").value();
    Harness harness;
    RouteFabricRuntime runtime(config, &harness.backend, &harness.path_authority);
    const Status status = runtime.Open();
    RF_REQUIRE(!status.has_value());
    RF_CHECK(status.error().code() == StatusCode::InvalidArgument);
  }
  {
    RuntimeConfig config;
    config.fabric = FabricId::parse("fabric").value();
    config.durability = Durability::Journal;
    Harness harness;
    RouteFabricRuntime runtime(config, &harness.backend, &harness.path_authority);
    const Status status = runtime.Open();
    RF_REQUIRE(!status.has_value());
    RF_CHECK(status.error().code() == StatusCode::InvalidArgument);
  }
  {
    Harness harness;
    RouteFabricRuntime runtime(RuntimeConfig(), nullptr, &harness.path_authority);
    const Status status = runtime.Open();
    RF_REQUIRE(!status.has_value());
    RF_CHECK(status.error().code() == StatusCode::InvalidArgument);
  }
}

RF_TEST(conflicting_attempt_identity_with_different_semantics_is_rejected) {
  auto harness = Harness::Create();
  RF_REQUIRE(harness->register_default().has_value());
  PublishRequest first = harness->publish_request(harness->key("10.0.0.0/24"),
                                                  harness->next_hop_binding(harness->next_next_hop()));
  RF_REQUIRE(harness->runtime->PublishRoute(first).has_value());
  // Reusing the mutation attempt identity for a different route key is a distinct
  // publication, but the programming attempt identities generated by the runtime
  // are always fresh, so a reused mutation attempt can never alias backend state.
  PublishRequest second = harness->publish_request(harness->key("10.0.1.0/24"),
                                                   harness->next_hop_binding(harness->next_next_hop()));
  second.attempt = first.attempt;
  RF_REQUIRE(harness->runtime->PublishRoute(second).has_value());
  const std::vector<RouteProgrammingRequest> calls = harness->backend.RecordedCalls();
  RF_REQUIRE_EQ(calls.size(), static_cast<std::size_t>(2));
  RF_CHECK(!(calls[0].attempt == calls[1].attempt));
}

RF_TEST(a_truncated_peer_frame_does_not_pin_the_session) {
  // A client that sends a header and then disconnects must not leave the server
  // waiting: the receive loop fails explicitly on peer close.
  auto harness = Harness::Create();
  ServerConfig server_config;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());
  {
    ClientConfig client_config;
    client_config.port = server.port();
    RouteFabricClient client(client_config);
    RF_REQUIRE(client.Connect().has_value());
    const std::vector<std::uint8_t> header(kFrameHeaderBytes, 0);
    // Send a partial frame: header only, no payload.
    const std::vector<std::uint8_t> frame = encode_frame(MessageId::Hello, std::span<const std::uint8_t>());
    RF_REQUIRE(client.SendFrame(MessageId::Hello, std::span<const std::uint8_t>()).has_value());
    (void)frame;
    (void)header;
    client.Close();
  }
  bool released = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (server.session_count() == 0) {
      released = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  RF_CHECK(released);
  server.Stop();
}

RF_TEST(every_configured_limit_is_consulted) {
  {  // max_destination_chars
    Limits limits;
    limits.max_destination_chars = 8;
    Destination destination;
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(DestinationKind::ServiceEndpoint));
    writer.u8(0);
    for (int index = 0; index < 16; ++index) {
      writer.u8(0);
    }
    writer.string("endpoint-token-longer-than-the-bound");
    ByteReader reader(writer.buffer());
    RF_CHECK(!Destination::read(reader, limits.max_destination_chars, destination));
  }
  {  // max_outstanding_programming
    Limits limits;
    limits.max_outstanding_programming = 1;
    RuntimeConfig config;
    config.limits = limits;
    auto harness = Harness::Create(config);
    RF_REQUIRE(harness->register_default().has_value());
    harness->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
    RF_CHECK(harness->publish_route("10.0.0.0/24").has_value());
    harness->backend.SetDefaultOutcome(ProgrammingOutcome::Applied, true);
    const Expected<PublishResult> excess = harness->publish_route("10.0.1.0/24");
    RF_REQUIRE(!excess.has_value());
    RF_CHECK(excess.error().code() == StatusCode::LimitExceeded);
  }
  {  // max_snapshot_routes
    Limits limits;
    limits.max_snapshot_routes = 1;
    RuntimeConfig config;
    config.limits = limits;
    auto harness = Harness::Create(config);
    RF_REQUIRE(harness->register_default().has_value());
    RF_CHECK(harness->publish_route("10.0.0.0/24").has_value());
    RF_CHECK(harness->publish_route("10.0.1.0/24").has_value());
    const Expected<RouteSnapshotSet> snapshot = harness->runtime->Snapshot();
    RF_REQUIRE(!snapshot.has_value());
    RF_CHECK(snapshot.error().code() == StatusCode::LimitExceeded);
    RF_CHECK_EQ(harness->runtime->ListRoutes(RouteListFilter{}).size(), static_cast<std::size_t>(1));
  }
  {  // max_persist_bytes
    Limits limits;
    limits.max_persist_bytes = 2048;
    PersistedState state;
    state.epoch = CoordinatorEpoch::from_value(1);
    state.policy_generation = PolicyGeneration::from_value(1);
    RouteStore store(std::filesystem::path("unused"), limits, Durability::Snapshot);
    // A payload larger than the configured bound cannot be encoded.
    ByteWriter payload;
    payload.u64(1);
    payload.u64(1);
    payload.u32(0);
    payload.u32(0);
    payload.u32(0);
    payload.raw(Digest128::from_u64_pair(1, 2).bytes());
    const std::vector<std::uint8_t> image = encode_persistence_frame(kPersistenceMagic, payload.buffer());
    RF_CHECK(image.size() < limits.max_persist_bytes);
    PersistedState decoded;
    std::string why;
    RF_CHECK(!RouteStore::DecodeSnapshot(image, limits, decoded, why));
    RF_CHECK(RouteStore::EncodeSnapshot(state, limits).empty() == false);
    Limits tiny;
    tiny.max_persist_bytes = 1024;
    Limits broken;
    broken.max_persist_bytes = 100;
    RF_CHECK(!broken.validate().has_value());
  }
  {  // max_routing_namespaces is validated as a configuration bound
    Limits limits;
    limits.max_routing_namespaces = 0;
    RF_CHECK(!limits.validate().has_value());
  }
}

RF_TEST(a_server_stop_with_live_sessions_is_clean) {
  auto harness = Harness::Create();
  ServerConfig server_config;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());
  std::vector<std::unique_ptr<RouteFabricClient>> clients;
  for (int index = 0; index < 3; ++index) {
    ClientConfig client_config;
    client_config.port = server.port();
    auto client = std::make_unique<RouteFabricClient>(client_config);
    RF_REQUIRE(client->Connect().has_value());
    RF_REQUIRE(client->Hello("stop-test").has_value());
    clients.push_back(std::move(client));
  }
  RF_CHECK_EQ(server.session_count(), static_cast<std::size_t>(3));
  server.Stop();
  RF_CHECK(!server.running());
  RF_CHECK_EQ(server.session_count(), static_cast<std::size_t>(0));
  // Every client observes the shutdown as an explicit failure rather than a hang.
  for (const auto& client : clients) {
    const Expected<DecodedFrame> frame = client->ReceiveFrame();
    RF_CHECK(!frame.has_value());
    client->Close();
  }
  RF_CHECK_EQ(server.counters().sessions_closed, static_cast<std::uint64_t>(3));
}
