#include "test_framework.hpp"

#include <atomic>
#include <thread>

#include "harness.hpp"
#include "routefabric/client.hpp"
#include "routefabric/server.hpp"
#include "routefabric/wire.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

MessageId message_id_at(std::size_t index) { return static_cast<MessageId>(index + 1); }

}  // namespace

RF_TEST(message_identifiers_are_stable) {
  RF_CHECK_EQ(static_cast<std::uint16_t>(MessageId::Hello), static_cast<std::uint16_t>(1));
  RF_CHECK_EQ(static_cast<std::uint16_t>(MessageId::PublishRoute), static_cast<std::uint16_t>(5));
  RF_CHECK_EQ(static_cast<std::uint16_t>(MessageId::SnapshotResponse), static_cast<std::uint16_t>(18));
  RF_CHECK_EQ(static_cast<std::uint16_t>(MessageId::ExplainRouteResult), static_cast<std::uint16_t>(31));
  for (std::size_t index = 0; index < 31; ++index) {
    const MessageId id = message_id_at(index);
    RF_CHECK(std::string(to_string(id)) != "UNKNOWN");
    RF_CHECK(is_message_id_value(static_cast<std::uint16_t>(id)));
  }
  RF_CHECK(!is_message_id_value(0));
  RF_CHECK(!is_message_id_value(99));
}

RF_TEST(frames_round_trip_and_validate_every_field) {
  Limits limits;
  const std::string payload = "payload-bytes";
  const std::vector<std::uint8_t> frame = encode_frame(
      MessageId::PublishRoute,
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
  RF_CHECK_EQ(frame.size(), kFrameHeaderBytes + payload.size());

  DecodedFrame decoded;
  std::string why;
  RF_CHECK(decode_frame(frame, limits, decoded, why) == FrameStatus::Complete);
  RF_CHECK(decoded.id == MessageId::PublishRoute);
  RF_CHECK_EQ(decoded.wire_version, kWireProtocolVersion);
  RF_CHECK_EQ(decoded.payload.size(), payload.size());
  RF_CHECK_EQ(std::string(decoded.payload.begin(), decoded.payload.end()), payload);

  {  // wrong magic
    std::vector<std::uint8_t> corrupted = frame;
    corrupted[0] ^= 0xFFu;
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::InvalidMagic);
  }
  {  // unsupported version
    std::vector<std::uint8_t> corrupted = frame;
    write_le16(corrupted.data() + 4, 99);
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::InvalidVersion);
  }
  {  // unknown message identifier
    std::vector<std::uint8_t> corrupted = frame;
    write_le16(corrupted.data() + 6, 4096);
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::InvalidMessageId);
  }
  {  // oversized payload declaration
    std::vector<std::uint8_t> corrupted = frame;
    write_le32(corrupted.data() + 8, static_cast<std::uint32_t>(limits.max_frame_bytes + 1));
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::OversizedPayload);
  }
  {  // trailing bytes
    std::vector<std::uint8_t> corrupted = frame;
    corrupted.push_back(0);
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::TrailingBytes);
  }
  {  // integrity failure in the payload
    std::vector<std::uint8_t> corrupted = frame;
    corrupted[kFrameHeaderBytes] ^= 0x20u;
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::IntegrityFailure);
  }
  {  // integrity covers the semantic header fields
    std::vector<std::uint8_t> corrupted = frame;
    write_le16(corrupted.data() + 6, static_cast<std::uint16_t>(MessageId::WithdrawRoute));
    DecodedFrame out;
    RF_CHECK(decode_frame(corrupted, limits, out, why) == FrameStatus::IntegrityFailure);
  }
  {  // truncation
    for (std::size_t length = 0; length < frame.size(); ++length) {
      const std::vector<std::uint8_t> truncated(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(length));
      DecodedFrame out;
      RF_CHECK(decode_frame(truncated, limits, out, why) == FrameStatus::Incomplete);
    }
  }
}

RF_TEST(message_codecs_round_trip) {
  Limits limits;
  const auto round_trip = [&limits](MessageId id, const std::vector<std::uint8_t>& payload) {
    const std::vector<std::uint8_t> frame = encode_frame(id, payload);
    DecodedFrame decoded;
    std::string why;
    RF_REQUIRE(decode_frame(frame, limits, decoded, why) == FrameStatus::Complete);
    RF_CHECK(decoded.id == id);
  };

  HelloRequest hello;
  hello.wire_version = kWireProtocolVersion;
  hello.client_name = "rf_tests";
  round_trip(MessageId::Hello, encode(hello, limits));
  HelloRequest hello_decoded;
  std::string why;
  RF_REQUIRE(decode(encode(hello, limits), limits, hello_decoded, why));
  RF_CHECK_EQ(hello_decoded.client_name, hello.client_name);

  HelloResult hello_result;
  hello_result.epoch = CoordinatorEpoch::from_value(4);
  hello_result.fabric = FabricId::parse("fabric").value();
  hello_result.backend = BackendId::parse("synthetic-test").value();
  hello_result.backend_read_only = true;
  hello_result.wire_version = kWireProtocolVersion;
  HelloResult hello_result_decoded;
  RF_REQUIRE(decode(encode(hello_result, limits), limits, hello_result_decoded, why));
  RF_CHECK(hello_result_decoded.epoch == hello_result.epoch);
  RF_CHECK(hello_result_decoded.backend_read_only);

  RegisterPublisherRequest register_request;
  register_request.context.epoch = CoordinatorEpoch::from_value(2);
  register_request.context.publisher = PublisherId::parse("publisher-a").value();
  register_request.context.worker_boot = make_id<WorkerBootId>(1);
  register_request.context.attempt = make_id<MutationAttemptId>(1);
  register_request.scope.fabric = FabricId::parse("fabric").value();
  register_request.scope.wildcard_namespaces = true;
  register_request.scope.destinations.push_back(Destination::parse_ipv4_prefix("10.0.0.0/8").value());
  register_request.scope.route_classes.push_back(RouteClass::Unicast);
  RegisterPublisherRequest register_decoded;
  RF_REQUIRE(decode(encode(register_request, limits), limits, register_decoded, why));
  RF_CHECK(register_decoded.context.publisher == register_request.context.publisher);
  RF_CHECK(register_decoded.scope.wildcard_namespaces);
  RF_CHECK_EQ(register_decoded.scope.destinations.size(), static_cast<std::size_t>(1));
  RF_CHECK(register_decoded.scope.destinations[0] == register_request.scope.destinations[0]);
  round_trip(MessageId::RegisterPublisher, encode(register_request, limits));

  PublishRouteRequest publish_request;
  publish_request.context.epoch = CoordinatorEpoch::from_value(2);
  publish_request.context.publisher = PublisherId::parse("publisher-a").value();
  publish_request.context.worker_boot = make_id<WorkerBootId>(1);
  publish_request.context.attempt = make_id<MutationAttemptId>(2);
  publish_request.key = Harness().key("10.0.0.0/24");
  publish_request.binding = Harness().next_hop_binding(make_id<NextHopId>(3));
  publish_request.policy_generation = PolicyGeneration::from_value(1);
  publish_request.expected_generation = RouteGeneration::from_value(3);
  publish_request.reason = "reason text";
  PublishRouteRequest publish_decoded;
  RF_REQUIRE(decode(encode(publish_request, limits), limits, publish_decoded, why));
  RF_CHECK(publish_decoded.key == publish_request.key);
  RF_CHECK(publish_decoded.binding == publish_request.binding);
  RF_CHECK(publish_decoded.expected_generation == publish_request.expected_generation);
  RF_CHECK_EQ(publish_decoded.reason, publish_request.reason);
  round_trip(MessageId::PublishRoute, encode(publish_request, limits));

  PublishRouteResult publish_result;
  publish_result.route = make_id<RouteId>(8);
  publish_result.generation = RouteGeneration::from_value(2);
  publish_result.authority_generation = RouteAuthorityGeneration::from_value(3);
  publish_result.lifecycle = RouteLifecycle::Installed;
  publish_result.applied = AppliedClassification::Applied;
  publish_result.currentness = RouteCurrentness::Current;
  publish_result.idempotent = true;
  PublishRouteResult publish_result_decoded;
  RF_REQUIRE(decode(encode(publish_result, limits), limits, publish_result_decoded, why));
  RF_CHECK(publish_result_decoded.route == publish_result.route);
  RF_CHECK(publish_result_decoded.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(publish_result_decoded.currentness == RouteCurrentness::Current);
  RF_CHECK(publish_result_decoded.idempotent);

  SnapshotResponse snapshot;
  snapshot.epoch = CoordinatorEpoch::from_value(1);
  snapshot.snapshot_id = make_id<RouteSnapshotId>(4);
  snapshot.digest = digest128("test", std::string_view("snapshot"));
  SnapshotResponse snapshot_decoded;
  RF_REQUIRE(decode(encode(snapshot, limits), limits, snapshot_decoded, why));
  RF_CHECK(snapshot_decoded.snapshot_id == snapshot.snapshot_id);

  ErrorMessage error;
  error.status = StatusCode::StaleEpoch;
  error.detail = "stale";
  ErrorMessage error_decoded;
  RF_REQUIRE(decode(encode(error, limits), limits, error_decoded, why));
  RF_CHECK(error_decoded.status == StatusCode::StaleEpoch);
  RF_CHECK_EQ(error_decoded.detail, std::string("stale"));
}

RF_TEST(codecs_reject_trailing_bytes_and_unknown_enumerations) {
  Limits limits;
  HelloRequest hello;
  hello.wire_version = kWireProtocolVersion;
  hello.client_name = "client";
  std::vector<std::uint8_t> payload = encode(hello, limits);
  payload.push_back(0);
  HelloRequest decoded;
  std::string why;
  RF_CHECK(!decode(payload, limits, decoded, why));

  // Unknown lifecycle value inside a publication result.
  PublishRouteResult result;
  std::vector<std::uint8_t> bytes = encode(result, limits);
  // The lifecycle byte sits after status(u16) + detail(u32 length) + route(16) +
  // generation(1+8) + authority generation(1+8).
  const std::size_t lifecycle_offset = 2 + 4 + 16 + 9 + 9;
  bytes[lifecycle_offset] = 200;
  PublishRouteResult decoded_result;
  RF_CHECK(!decode(bytes, limits, decoded_result, why));

  // Unknown status code.
  ErrorMessage error;
  std::vector<std::uint8_t> error_bytes = encode(error, limits);
  error_bytes[0] = 0xFFu;
  error_bytes[1] = 0xFFu;
  ErrorMessage decoded_error;
  RF_CHECK(!decode(error_bytes, limits, decoded_error, why));
}

RF_TEST(codecs_reject_oversized_strings_and_counts) {
  Limits limits;
  // A hello with a 200 byte client name exceeds the bound.
  ByteWriter writer;
  writer.u16(kWireProtocolVersion);
  writer.string(std::string(200, 'a'));
  HelloRequest decoded;
  std::string why;
  RF_CHECK(!decode(writer.buffer(), limits, decoded, why));

  // A registration scope with more entries than the configured bound.
  Limits tiny;
  tiny.max_publisher_scopes = 2;
  RegisterPublisherRequest request;
  request.context.epoch = CoordinatorEpoch::from_value(1);
  request.context.publisher = PublisherId::parse("publisher-a").value();
  request.context.worker_boot = make_id<WorkerBootId>(1);
  request.context.attempt = make_id<MutationAttemptId>(1);
  request.scope.fabric = FabricId::parse("fabric").value();
  for (int index = 0; index < 5; ++index) {
    request.scope.namespaces.push_back(RoutingNamespace::parse("ns" + to_decimal(static_cast<std::uint64_t>(index))).value());
  }
  RegisterPublisherRequest decoded_request;
  RF_CHECK(!decode(encode(request, limits), tiny, decoded_request, why));
}

RF_TEST(protocol_round_trip_over_real_loopback_tcp) {
  auto harness = Harness::Create();
  ServerConfig server_config;
  server_config.port = 0;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());
  RF_CHECK(server.port() != 0);

  ClientConfig client_config;
  client_config.port = server.port();
  client_config.id_seed = 99;
  RouteFabricClient client(client_config);
  RF_REQUIRE(client.Connect().has_value());
  const Expected<HelloResult> hello = client.Hello("rf_tests");
  RF_REQUIRE(hello.has_value());
  RF_CHECK(hello.value().status == StatusCode::Ok);
  RF_CHECK(hello.value().epoch == CoordinatorEpoch::from_value(1));
  RF_CHECK_EQ(hello.value().backend.render(), std::string("synthetic-test"));

  client.context().publisher = PublisherId::parse("publisher-a").value();
  client.context().worker_boot = make_id<WorkerBootId>(1);
  PublisherScope scope;
  scope.fabric = FabricId::parse("fabric").value();
  scope.wildcard_namespaces = true;
  scope.wildcard_destinations = true;
  scope.wildcard_route_classes = true;
  client.next_attempt();
  const Expected<RegisterPublisherResult> registration = client.RegisterPublisher(scope);
  RF_REQUIRE(registration.has_value());
  RF_CHECK(registration.value().status == StatusCode::Ok);

  client.next_attempt();
  const RouteKey key = harness->key("10.0.0.0/24");
  const RouteBinding binding = harness->next_hop_binding(make_id<NextHopId>(5));
  const Expected<PublishRouteResult> published =
      client.PublishRoute(key, binding, PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "over the wire");
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().status == StatusCode::Ok);
  RF_CHECK(published.value().lifecycle == RouteLifecycle::Installed);
  RF_CHECK(published.value().currentness == RouteCurrentness::Current);

  const Expected<RouteSnapshot> queried = client.QueryRouteByKey(key);
  RF_REQUIRE(queried.has_value());
  RF_CHECK(queried.value().record.binding == binding);
  const Expected<RouteExplanation> explained = client.ExplainRouteByKey(key);
  RF_REQUIRE(explained.has_value());
  RF_CHECK(explained.value().authoritative);

  const Expected<SnapshotResponse> snapshot = client.Snapshot();
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK_EQ(snapshot.value().routes.size(), static_cast<std::size_t>(1));
  RF_CHECK(snapshot.value().digest.is_zero() == false);

  const Expected<StatisticsResult> statistics = client.Statistics();
  RF_REQUIRE(statistics.has_value());
  RF_CHECK_EQ(statistics.value().route_count, static_cast<std::uint64_t>(1));

  const Expected<BackendStateMessage> backend_state = client.BackendState();
  RF_REQUIRE(backend_state.has_value());
  RF_CHECK(backend_state.value().capabilities.install);
  RF_CHECK(!backend_state.value().capabilities.read_only);

  client.next_attempt();
  const Expected<RouteMutationResult> withdrawn = client.WithdrawRoute(published.value().route, RouteGeneration(), "over the wire");
  RF_REQUIRE(withdrawn.has_value());
  RF_CHECK(withdrawn.value().status == StatusCode::Ok);
  RF_CHECK(withdrawn.value().lifecycle == RouteLifecycle::Withdrawn);

  client.Close();
  server.Stop();
  RF_CHECK(!server.running());
  RF_CHECK_EQ(server.session_count(), static_cast<std::size_t>(0));
}

RF_TEST(a_malformed_peer_is_rejected_and_disconnected) {
  auto harness = Harness::Create();
  ServerConfig server_config;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());

  ClientConfig client_config;
  client_config.port = server.port();
  RouteFabricClient client(client_config);
  RF_REQUIRE(client.Connect().has_value());
  RF_REQUIRE(client.Hello("rf_tests").has_value());

  // A frame whose integrity field does not match its payload.
  HelloRequest hello;
  hello.wire_version = kWireProtocolVersion;
  hello.client_name = "malformed";
  std::vector<std::uint8_t> frame = encode_frame(MessageId::Hello, encode(hello, Limits()));
  frame[12] ^= 0xFFu;
  RF_REQUIRE(client.SendRawBytes(frame).has_value());
  const Expected<DecodedFrame> response = client.ReceiveFrame();
  RF_REQUIRE(response.has_value());
  RF_CHECK(response.value().id == MessageId::Error);
  ErrorMessage error;
  std::string why;
  RF_REQUIRE(decode(response.value().payload, Limits(), error, why));
  RF_CHECK(error.status == StatusCode::IntegrityFailure);

  // The session is closed after a protocol violation.
  const Expected<DecodedFrame> after = client.ReceiveFrame();
  RF_CHECK(!after.has_value());
  client.Close();
  server.Stop();
}

RF_TEST(an_oversized_frame_is_refused_before_it_is_buffered) {
  auto harness = Harness::Create();
  Limits limits;
  limits.max_frame_bytes = 128;
  ServerConfig server_config;
  server_config.limits = limits;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());

  ClientConfig client_config;
  client_config.port = server.port();
  client_config.limits = limits;
  RouteFabricClient client(client_config);
  RF_REQUIRE(client.Connect().has_value());

  // A well-formed header that declares a payload beyond the configured bound.
  // Only the header is sent: the server must refuse it without waiting for a body.
  std::vector<std::uint8_t> header(kFrameHeaderBytes, 0);
  write_le32(header.data(), kWireFrameMagic);
  write_le16(header.data() + 4, kWireProtocolVersion);
  write_le16(header.data() + 6, static_cast<std::uint16_t>(MessageId::Hello));
  write_le32(header.data() + 8, 4096);
  Crc32c crc;
  crc.update(std::span<const std::uint8_t>(header.data(), kFrameHeaderBytes - 4));
  write_le32(header.data() + 12, crc.value());
  RF_REQUIRE(client.SendRawBytes(header).has_value());
  const Expected<DecodedFrame> response = client.ReceiveFrame();
  RF_REQUIRE(response.has_value());
  RF_CHECK(response.value().id == MessageId::Error);
  client.Close();

  const ServerCounters counters = server.counters();
  RF_CHECK_EQ(counters.oversized_frames, static_cast<std::uint64_t>(1));
  server.Stop();
}

RF_TEST(sessions_are_bounded_and_released) {
  auto harness = Harness::Create();
  Limits limits;
  limits.max_sessions = 1;
  ServerConfig server_config;
  server_config.limits = limits;
  RouteFabricServer server(server_config, harness->runtime.get(), &harness->backend);
  RF_REQUIRE(server.Start().has_value());

  ClientConfig first_config;
  first_config.port = server.port();
  RouteFabricClient first(first_config);
  RF_REQUIRE(first.Connect().has_value());
  RF_REQUIRE(first.Hello("first").has_value());

  ClientConfig second_config;
  second_config.port = server.port();
  RouteFabricClient second(second_config);
  RF_REQUIRE(second.Connect().has_value());
  const Expected<HelloResult> refused = second.Hello("second");
  RF_CHECK(!refused.has_value());
  second.Close();

  first.Close();
  // The server releases the session slot once the connection ends.
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
