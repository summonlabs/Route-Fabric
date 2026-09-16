#include "routefabric/wire.hpp"

#include <array>
#include <cstring>

#include "routefabric/hash.hpp"
#include "routefabric/version.hpp"

namespace routefabric {
namespace {

constexpr std::uint16_t kMaxStatusCodeValue = static_cast<std::uint16_t>(StatusCode::NotOpen);

template <typename Id>
void write_optional_fixed(ByteWriter& writer, const Id& id) {
  writer.raw(id.bytes());
}

template <typename Id>
bool read_optional_fixed(ByteReader& reader, Id& out) {
  static_assert(Id::kByteCount == 16, "optional fixed identities are 128-bit");
  std::span<const std::uint8_t> raw;
  if (!reader.raw(Id::kByteCount, raw)) {
    return false;
  }
  std::array<std::uint8_t, Id::kByteCount> bytes{};
  for (std::size_t i = 0; i < Id::kByteCount; ++i) {
    bytes[i] = raw[i];
  }
  out = Id(bytes);
  return true;
}

template <typename Id>
void write_optional_counter(ByteWriter& writer, const Id& id) {
  if (id.is_valid()) {
    writer.u8(1);
    id.write(writer);
  } else {
    writer.u8(0);
    writer.u64(0);
  }
}

template <typename Id>
bool read_optional_counter(ByteReader& reader, Id& out) {
  std::uint8_t present = 0;
  std::uint64_t raw = 0;
  if (!reader.u8(present) || !reader.u64(raw)) {
    return false;
  }
  if (present == 0) {
    if (raw != 0) {
      return false;
    }
    out = Id();
    return true;
  }
  if (present != 1 || raw == 0 || raw > Id::kMaxValue) {
    return false;
  }
  out = Id::from_value(raw);
  return true;
}

void write_status(ByteWriter& writer, StatusCode code, std::string_view detail) {
  writer.u16(static_cast<std::uint16_t>(code));
  writer.string(detail);
}

bool read_status(ByteReader& reader, StatusCode& code, std::string& detail) {
  std::uint16_t raw = 0;
  if (!reader.u16(raw)) {
    return false;
  }
  if (raw > kMaxStatusCodeValue) {
    return false;
  }
  code = static_cast<StatusCode>(raw);
  return reader.string(kMaxDiagnosticChars, detail);
}

void write_context(ByteWriter& writer, const RequestContext& context) {
  context.epoch.write(writer);
  context.publisher.write(writer);
  context.worker_boot.write(writer);
  context.attempt.write(writer);
}

bool read_context(ByteReader& reader, RequestContext& context) {
  if (!CoordinatorEpoch::read(reader, context.epoch)) {
    return false;
  }
  if (!PublisherId::read(reader, context.publisher)) {
    return false;
  }
  if (!WorkerBootId::read(reader, context.worker_boot)) {
    return false;
  }
  return MutationAttemptId::read(reader, context.attempt);
}

void write_counter_fields(ByteWriter& writer, const RouteCounters& counters) {
  writer.u64(counters.publications);
  writer.u64(counters.replacements);
  writer.u64(counters.idempotent_publications);
  writer.u64(counters.withdrawals);
  writer.u64(counters.revalidations);
  writer.u64(counters.retirements);
  writer.u64(counters.revocations);
  writer.u64(counters.supersessions);
  writer.u64(counters.programming_dispatches);
  writer.u64(counters.programming_deferred);
  writer.u64(counters.programming_applied);
  writer.u64(counters.programming_ambiguous);
  writer.u64(counters.programming_lifecycle_rejected);
  writer.u64(counters.stale_completions_rejected);
  writer.u64(counters.stale_authority_rejections);
  writer.u64(counters.scope_violations);
  writer.u64(counters.path_authority_rejections);
  writer.u64(counters.conflicts);
  writer.u64(counters.reconciliations);
}

bool read_counter_fields(ByteReader& reader, RouteCounters& counters) {
  std::uint64_t values[19] = {};
  for (std::uint64_t& value : values) {
    if (!reader.u64(value)) {
      return false;
    }
  }
  counters.publications = values[0];
  counters.replacements = values[1];
  counters.idempotent_publications = values[2];
  counters.withdrawals = values[3];
  counters.revalidations = values[4];
  counters.retirements = values[5];
  counters.revocations = values[6];
  counters.supersessions = values[7];
  counters.programming_dispatches = values[8];
  counters.programming_deferred = values[9];
  counters.programming_applied = values[10];
  counters.programming_ambiguous = values[11];
  counters.programming_lifecycle_rejected = values[12];
  counters.stale_completions_rejected = values[13];
  counters.stale_authority_rejections = values[14];
  counters.scope_violations = values[15];
  counters.path_authority_rejections = values[16];
  counters.conflicts = values[17];
  counters.reconciliations = values[18];
  return true;
}

}  // namespace

const char* to_string(MessageId id) noexcept {
  switch (id) {
    case MessageId::Hello:
      return "HELLO";
    case MessageId::HelloResult:
      return "HELLO_RESULT";
    case MessageId::RegisterPublisher:
      return "REGISTER_PUBLISHER";
    case MessageId::RegisterPublisherResult:
      return "REGISTER_PUBLISHER_RESULT";
    case MessageId::PublishRoute:
      return "PUBLISH_ROUTE";
    case MessageId::PublishRouteResult:
      return "PUBLISH_ROUTE_RESULT";
    case MessageId::WithdrawRoute:
      return "WITHDRAW_ROUTE";
    case MessageId::WithdrawRouteResult:
      return "WITHDRAW_ROUTE_RESULT";
    case MessageId::RevalidateRoute:
      return "REVALIDATE_ROUTE";
    case MessageId::RevalidateRouteResult:
      return "REVALIDATE_ROUTE_RESULT";
    case MessageId::RetireRoute:
      return "RETIRE_ROUTE";
    case MessageId::RetireRouteResult:
      return "RETIRE_ROUTE_RESULT";
    case MessageId::RevokeRoute:
      return "REVOKE_ROUTE";
    case MessageId::RevokeRouteResult:
      return "REVOKE_ROUTE_RESULT";
    case MessageId::QueryRoute:
      return "QUERY_ROUTE";
    case MessageId::QueryRouteResult:
      return "QUERY_ROUTE_RESULT";
    case MessageId::SnapshotRequest:
      return "SNAPSHOT_REQUEST";
    case MessageId::SnapshotResponse:
      return "SNAPSHOT_RESPONSE";
    case MessageId::BackendState:
      return "BACKEND_STATE";
    case MessageId::FenceNotice:
      return "FENCE_NOTICE";
    case MessageId::Error:
      return "ERROR";
    case MessageId::ReconcileRequest:
      return "RECONCILE_REQUEST";
    case MessageId::ReconcileResult:
      return "RECONCILE_RESULT";
    case MessageId::EpochRequest:
      return "EPOCH_REQUEST";
    case MessageId::EpochResult:
      return "EPOCH_RESULT";
    case MessageId::StatisticsRequest:
      return "STATISTICS_REQUEST";
    case MessageId::StatisticsResult:
      return "STATISTICS_RESULT";
    case MessageId::ApplyCompletion:
      return "APPLY_COMPLETION";
    case MessageId::ApplyCompletionResult:
      return "APPLY_COMPLETION_RESULT";
    case MessageId::ExplainRoute:
      return "EXPLAIN_ROUTE";
    case MessageId::ExplainRouteResult:
      return "EXPLAIN_ROUTE_RESULT";
  }
  return "UNKNOWN";
}

bool is_message_id_value(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(MessageId::Hello) &&
         raw <= static_cast<std::uint16_t>(MessageId::ExplainRouteResult);
}

std::vector<std::uint8_t> encode_frame(MessageId id, std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> frame(kFrameHeaderBytes + payload.size());
  write_le32(frame.data(), kWireFrameMagic);
  write_le16(frame.data() + 4, kWireProtocolVersion);
  write_le16(frame.data() + 6, static_cast<std::uint16_t>(id));
  write_le32(frame.data() + 8, static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty()) {
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
  }
  Crc32c crc;
  crc.update(std::span<const std::uint8_t>(frame.data(), kFrameHeaderBytes - 4));
  crc.update(payload);
  write_le32(frame.data() + 12, crc.value());
  return frame;
}

const char* to_string(FrameStatus status) noexcept {
  switch (status) {
    case FrameStatus::Complete:
      return "COMPLETE";
    case FrameStatus::Incomplete:
      return "INCOMPLETE";
    case FrameStatus::InvalidMagic:
      return "INVALID_MAGIC";
    case FrameStatus::InvalidVersion:
      return "INVALID_VERSION";
    case FrameStatus::InvalidMessageId:
      return "INVALID_MESSAGE_ID";
    case FrameStatus::OversizedPayload:
      return "OVERSIZED_PAYLOAD";
    case FrameStatus::TrailingBytes:
      return "TRAILING_BYTES";
    case FrameStatus::IntegrityFailure:
      return "INTEGRITY_FAILURE";
  }
  return "UNKNOWN";
}

bool is_complete(FrameStatus status) noexcept { return status == FrameStatus::Complete; }

bool is_invalid(FrameStatus status) noexcept {
  return status != FrameStatus::Complete && status != FrameStatus::Incomplete;
}

FrameStatus frame_header_status(std::span<const std::uint8_t> buffer, const Limits& limits, std::size_t& total,
                                std::string& why) {
  if (buffer.size() < kFrameHeaderBytes) {
    why = "incomplete frame header";
    return FrameStatus::Incomplete;
  }
  if (read_le32(buffer.data()) != kWireFrameMagic) {
    why = "wrong frame magic";
    return FrameStatus::InvalidMagic;
  }
  if (read_le16(buffer.data() + 4) != kWireProtocolVersion) {
    why = "unsupported wire protocol version";
    return FrameStatus::InvalidVersion;
  }
  const std::uint16_t raw_message = read_le16(buffer.data() + 6);
  if (!is_message_id_value(raw_message)) {
    why = "unknown message identifier";
    return FrameStatus::InvalidMessageId;
  }
  const std::uint32_t payload_length = read_le32(buffer.data() + 8);
  if (static_cast<std::size_t>(payload_length) > limits.max_frame_bytes) {
    why = "frame payload exceeds the configured bound";
    return FrameStatus::OversizedPayload;
  }
  total = kFrameHeaderBytes + static_cast<std::size_t>(payload_length);
  return FrameStatus::Complete;
}

FrameStatus decode_frame(std::span<const std::uint8_t> buffer, const Limits& limits, DecodedFrame& out,
                         std::string& why) {
  std::size_t total = 0;
  const FrameStatus header_status = frame_header_status(buffer, limits, total, why);
  if (header_status != FrameStatus::Complete) {
    return header_status;
  }
  if (buffer.size() < total) {
    why = "incomplete frame payload";
    return FrameStatus::Incomplete;
  }
  if (buffer.size() > total) {
    why = "trailing bytes after the frame payload";
    return FrameStatus::TrailingBytes;
  }
  const std::uint32_t payload_length = read_le32(buffer.data() + 8);
  const std::span<const std::uint8_t> payload = buffer.subspan(kFrameHeaderBytes, payload_length);
  Crc32c crc;
  crc.update(std::span<const std::uint8_t>(buffer.data(), kFrameHeaderBytes - 4));
  crc.update(payload);
  if (crc.value() != read_le32(buffer.data() + 12)) {
    why = "frame integrity check failed";
    return FrameStatus::IntegrityFailure;
  }
  out.id = static_cast<MessageId>(read_le16(buffer.data() + 6));
  out.wire_version = read_le16(buffer.data() + 4);
  out.payload.assign(payload.begin(), payload.end());
  return FrameStatus::Complete;
}

// ---------------------------------------------------------------------------
// Shared codecs
// ---------------------------------------------------------------------------

void write_publisher_scope(ByteWriter& writer, const PublisherScope& scope) {
  scope.fabric.write(writer);
  writer.u32(static_cast<std::uint32_t>(scope.namespaces.size()));
  for (const RoutingNamespace& item : scope.namespaces) {
    item.write(writer);
  }
  writer.u32(static_cast<std::uint32_t>(scope.destinations.size()));
  for (const Destination& item : scope.destinations) {
    item.write(writer);
  }
  writer.u32(static_cast<std::uint32_t>(scope.route_classes.size()));
  for (const RouteClass item : scope.route_classes) {
    writer.u8(static_cast<std::uint8_t>(item));
  }
  writer.boolean(scope.wildcard_namespaces);
  writer.boolean(scope.wildcard_destinations);
  writer.boolean(scope.wildcard_route_classes);
  writer.boolean(scope.administrative_override);
}

bool read_publisher_scope(ByteReader& reader, const Limits& limits, PublisherScope& out) {
  PublisherScope scope;
  if (!FabricId::read(reader, scope.fabric)) {
    return false;
  }
  std::uint32_t namespace_count = 0;
  if (!reader.u32(namespace_count) || static_cast<std::size_t>(namespace_count) > limits.max_publisher_scopes) {
    return false;
  }
  scope.namespaces.reserve(namespace_count);
  for (std::uint32_t i = 0; i < namespace_count; ++i) {
    RoutingNamespace item;
    if (!RoutingNamespace::read(reader, item)) {
      return false;
    }
    scope.namespaces.push_back(std::move(item));
  }
  std::uint32_t destination_count = 0;
  if (!reader.u32(destination_count) || static_cast<std::size_t>(destination_count) > limits.max_publisher_scopes) {
    return false;
  }
  scope.destinations.reserve(destination_count);
  for (std::uint32_t i = 0; i < destination_count; ++i) {
    Destination item;
    if (!Destination::read(reader, limits.max_destination_chars, item)) {
      return false;
    }
    scope.destinations.push_back(std::move(item));
  }
  std::uint32_t class_count = 0;
  if (!reader.u32(class_count) || static_cast<std::size_t>(class_count) > limits.max_publisher_scopes) {
    return false;
  }
  scope.route_classes.reserve(class_count);
  for (std::uint32_t i = 0; i < class_count; ++i) {
    std::uint8_t raw = 0;
    if (!reader.u8(raw)) {
      return false;
    }
    if (raw < static_cast<std::uint8_t>(RouteClass::Unicast) ||
        raw > static_cast<std::uint8_t>(RouteClass::Logical)) {
      return false;
    }
    scope.route_classes.push_back(static_cast<RouteClass>(raw));
  }
  if (!reader.boolean(scope.wildcard_namespaces) || !reader.boolean(scope.wildcard_destinations) ||
      !reader.boolean(scope.wildcard_route_classes) || !reader.boolean(scope.administrative_override)) {
    return false;
  }
  out = std::move(scope);
  return true;
}

void write_route_snapshot(ByteWriter& writer, const RouteSnapshot& snapshot) {
  snapshot.id.write(writer);
  writer.raw(snapshot.digest.bytes());
  writer.u8(static_cast<std::uint8_t>(snapshot.currentness));
  write_route_record(writer, snapshot.record);
}

bool read_route_snapshot(ByteReader& reader, const Limits& limits, RouteSnapshot& out, std::string& why) {
  RouteSnapshot snapshot;
  if (!RouteSnapshotId::read(reader, snapshot.id)) {
    why = "malformed snapshot identity";
    return false;
  }
  std::span<const std::uint8_t> digest_bytes;
  if (!reader.raw(16, digest_bytes)) {
    why = "malformed snapshot digest";
    return false;
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    bytes[i] = digest_bytes[i];
  }
  snapshot.digest = Digest128(bytes);
  std::uint8_t currentness = 0;
  if (!reader.u8(currentness) ||
      currentness < static_cast<std::uint8_t>(RouteCurrentness::Current) ||
      currentness > static_cast<std::uint8_t>(RouteCurrentness::DesiredAppliedMismatch)) {
    why = "malformed currentness value";
    return false;
  }
  snapshot.currentness = static_cast<RouteCurrentness>(currentness);
  if (!read_route_record(reader, limits, snapshot.record, why)) {
    return false;
  }
  if (!(semantic_digest(snapshot.record) == snapshot.digest)) {
    why = "snapshot digest does not match the record content";
    return false;
  }
  out = std::move(snapshot);
  return true;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode(const HelloRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  writer.u16(message.wire_version);
  writer.string(message.client_name);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, HelloRequest& out, std::string& why) {
  ByteReader reader(payload);
  HelloRequest message;
  if (!reader.u16(message.wire_version)) {
    why = "malformed hello version";
    return false;
  }
  if (message.wire_version != kWireProtocolVersion) {
    why = "unsupported wire protocol version in hello";
    return false;
  }
  if (!reader.string(64, message.client_name)) {
    why = "malformed client name";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the hello payload";
    return false;
  }
  (void)limits;
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const HelloResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  writer.u16(message.wire_version);
  message.epoch.write(writer);
  message.fabric.write(writer);
  message.backend.write(writer);
  writer.boolean(message.backend_read_only);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, HelloResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  HelloResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed hello result status";
    return false;
  }
  if (!reader.u16(message.wire_version)) {
    why = "malformed hello result version";
    return false;
  }
  if (message.wire_version != kWireProtocolVersion) {
    why = "unsupported wire protocol version in hello result";
    return false;
  }
  if (!CoordinatorEpoch::read(reader, message.epoch)) {
    why = "malformed hello result epoch";
    return false;
  }
  if (!FabricId::read(reader, message.fabric)) {
    why = "malformed hello result fabric";
    return false;
  }
  if (!BackendId::read(reader, message.backend)) {
    why = "malformed hello result backend";
    return false;
  }
  if (!reader.boolean(message.backend_read_only)) {
    why = "malformed hello result capability flag";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the hello result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const RegisterPublisherRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_context(writer, message.context);
  write_publisher_scope(writer, message.scope);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RegisterPublisherRequest& out,
            std::string& why) {
  ByteReader reader(payload);
  RegisterPublisherRequest message;
  if (!read_context(reader, message.context)) {
    why = "malformed registration authority context";
    return false;
  }
  if (!read_publisher_scope(reader, limits, message.scope)) {
    why = "malformed publisher scope";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the registration payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const RegisterPublisherResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  write_optional_counter(writer, message.epoch);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RegisterPublisherResult& out,
            std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  RegisterPublisherResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed registration result status";
    return false;
  }
  if (!read_optional_counter(reader, message.epoch)) {
    why = "malformed registration result epoch";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the registration result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const PublishRouteRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_context(writer, message.context);
  message.key.write(writer);
  write_route_binding(writer, message.binding);
  message.policy_generation.write(writer);
  write_optional_counter(writer, message.expected_generation);
  write_optional_fixed(writer, message.lineage);
  writer.string(message.reason);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, PublishRouteRequest& out, std::string& why) {
  ByteReader reader(payload);
  PublishRouteRequest message;
  if (!read_context(reader, message.context)) {
    why = "malformed publication authority context";
    return false;
  }
  if (!RouteKey::read(reader, limits.max_destination_chars, message.key)) {
    why = "malformed route key";
    return false;
  }
  if (!read_route_binding(reader, message.binding)) {
    why = "malformed desired binding";
    return false;
  }
  if (!PolicyGeneration::read(reader, message.policy_generation)) {
    why = "malformed policy generation";
    return false;
  }
  if (!read_optional_counter(reader, message.expected_generation)) {
    why = "malformed expected generation";
    return false;
  }
  if (!read_optional_fixed(reader, message.lineage)) {
    why = "malformed lineage identity";
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, message.reason)) {
    why = "malformed publication reason";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the publication payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const PublishRouteResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  write_optional_fixed(writer, message.route);
  write_optional_counter(writer, message.generation);
  write_optional_counter(writer, message.authority_generation);
  writer.u8(static_cast<std::uint8_t>(message.lifecycle));
  writer.u8(static_cast<std::uint8_t>(message.applied));
  writer.u8(static_cast<std::uint8_t>(message.currentness));
  writer.boolean(message.idempotent);
  writer.boolean(message.replaced);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, PublishRouteResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  PublishRouteResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed publication result status";
    return false;
  }
  if (!read_optional_fixed(reader, message.route)) {
    why = "malformed publication result route identity";
    return false;
  }
  if (!read_optional_counter(reader, message.generation) ||
      !read_optional_counter(reader, message.authority_generation)) {
    why = "malformed publication result generation";
    return false;
  }
  std::uint8_t lifecycle = 0;
  std::uint8_t applied = 0;
  std::uint8_t currentness = 0;
  if (!reader.u8(lifecycle) || !reader.u8(applied) || !reader.u8(currentness)) {
    why = "malformed publication result enumeration";
    return false;
  }
  if (!is_lifecycle_value(lifecycle) ||
      applied < static_cast<std::uint8_t>(AppliedClassification::Unknown) ||
      applied > static_cast<std::uint8_t>(AppliedClassification::BackendUnavailable) ||
      currentness < static_cast<std::uint8_t>(RouteCurrentness::Current) ||
      currentness > static_cast<std::uint8_t>(RouteCurrentness::DesiredAppliedMismatch)) {
    why = "unknown enumeration value in the publication result";
    return false;
  }
  message.lifecycle = static_cast<RouteLifecycle>(lifecycle);
  message.applied = static_cast<AppliedClassification>(applied);
  message.currentness = static_cast<RouteCurrentness>(currentness);
  if (!reader.boolean(message.idempotent) || !reader.boolean(message.replaced)) {
    why = "malformed publication result flags";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the publication result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const RouteMutationRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_context(writer, message.context);
  write_optional_fixed(writer, message.route);
  write_optional_counter(writer, message.expected_generation);
  writer.string(message.reason);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RouteMutationRequest& out,
            std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  RouteMutationRequest message;
  if (!read_context(reader, message.context)) {
    why = "malformed mutation authority context";
    return false;
  }
  if (!read_optional_fixed(reader, message.route)) {
    why = "malformed route identity";
    return false;
  }
  if (!read_optional_counter(reader, message.expected_generation)) {
    why = "malformed expected generation";
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, message.reason)) {
    why = "malformed mutation reason";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the mutation payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const RouteMutationResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  write_optional_fixed(writer, message.route);
  write_optional_counter(writer, message.generation);
  writer.u8(static_cast<std::uint8_t>(message.lifecycle));
  writer.u8(static_cast<std::uint8_t>(message.applied));
  writer.boolean(message.already_withdrawn);
  writer.boolean(message.withdrawal_in_flight);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RouteMutationResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  RouteMutationResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed mutation result status";
    return false;
  }
  if (!read_optional_fixed(reader, message.route)) {
    why = "malformed mutation result route identity";
    return false;
  }
  if (!read_optional_counter(reader, message.generation)) {
    why = "malformed mutation result generation";
    return false;
  }
  std::uint8_t lifecycle = 0;
  std::uint8_t applied = 0;
  if (!reader.u8(lifecycle) || !reader.u8(applied)) {
    why = "malformed mutation result enumeration";
    return false;
  }
  if (!is_lifecycle_value(lifecycle) ||
      applied < static_cast<std::uint8_t>(AppliedClassification::Unknown) ||
      applied > static_cast<std::uint8_t>(AppliedClassification::BackendUnavailable)) {
    why = "unknown enumeration value in the mutation result";
    return false;
  }
  message.lifecycle = static_cast<RouteLifecycle>(lifecycle);
  message.applied = static_cast<AppliedClassification>(applied);
  if (!reader.boolean(message.already_withdrawn) || !reader.boolean(message.withdrawal_in_flight)) {
    why = "malformed mutation result flag";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the mutation result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const QueryRouteRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  writer.boolean(message.by_key);
  message.key.write(writer);
  write_optional_fixed(writer, message.route);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, QueryRouteRequest& out, std::string& why) {
  ByteReader reader(payload);
  QueryRouteRequest message;
  if (!reader.boolean(message.by_key)) {
    why = "malformed query selector";
    return false;
  }
  if (!RouteKey::read(reader, limits.max_destination_chars, message.key)) {
    why = "malformed query route key";
    return false;
  }
  if (!read_optional_fixed(reader, message.route)) {
    why = "malformed query route identity";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the query payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const QueryRouteResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  writer.boolean(message.found);
  write_route_snapshot(writer, message.snapshot);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, QueryRouteResult& out, std::string& why) {
  ByteReader reader(payload);
  QueryRouteResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed query result status";
    return false;
  }
  if (!reader.boolean(message.found)) {
    why = "malformed query result flag";
    return false;
  }
  if (!read_route_snapshot(reader, limits, message.snapshot, why)) {
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the query result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const SnapshotRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_optional_counter(writer, message.epoch);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, SnapshotRequest& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  SnapshotRequest message;
  if (!read_optional_counter(reader, message.epoch)) {
    why = "malformed snapshot request epoch";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the snapshot request payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const SnapshotResponse& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  message.epoch.write(writer);
  message.snapshot_id.write(writer);
  writer.raw(message.digest.bytes());
  writer.u32(static_cast<std::uint32_t>(message.routes.size()));
  for (const RouteSnapshot& snapshot : message.routes) {
    write_route_snapshot(writer, snapshot);
  }
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, SnapshotResponse& out, std::string& why) {
  ByteReader reader(payload);
  SnapshotResponse message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed snapshot response status";
    return false;
  }
  if (!CoordinatorEpoch::read(reader, message.epoch)) {
    why = "malformed snapshot response epoch";
    return false;
  }
  if (!RouteSnapshotId::read(reader, message.snapshot_id)) {
    why = "malformed snapshot response identity";
    return false;
  }
  std::span<const std::uint8_t> digest_bytes;
  if (!reader.raw(16, digest_bytes)) {
    why = "malformed snapshot response digest";
    return false;
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    bytes[i] = digest_bytes[i];
  }
  message.digest = Digest128(bytes);
  std::uint32_t route_count = 0;
  if (!reader.u32(route_count)) {
    why = "malformed snapshot response count";
    return false;
  }
  if (static_cast<std::size_t>(route_count) > limits.max_snapshot_routes) {
    why = "snapshot response exceeds the configured route bound";
    return false;
  }
  message.routes.reserve(route_count);
  for (std::uint32_t i = 0; i < route_count; ++i) {
    RouteSnapshot snapshot;
    if (!read_route_snapshot(reader, limits, snapshot, why)) {
      return false;
    }
    message.routes.push_back(std::move(snapshot));
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the snapshot response payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const BackendStateMessage& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  message.backend.write(writer);
  writer.boolean(message.capabilities.install);
  writer.boolean(message.capabilities.replace);
  writer.boolean(message.capabilities.withdraw);
  writer.boolean(message.capabilities.query);
  writer.boolean(message.capabilities.read_only);
  writer.boolean(message.evidence_included);
  if (message.evidence_included) {
    message.key.write(writer);
    writer.u8(static_cast<std::uint8_t>(message.presence));
    writer.string(message.evidence);
  }
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, BackendStateMessage& out,
            std::string& why) {
  ByteReader reader(payload);
  BackendStateMessage message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed backend state status";
    return false;
  }
  if (!BackendId::read(reader, message.backend)) {
    why = "malformed backend identity";
    return false;
  }
  if (!reader.boolean(message.capabilities.install) || !reader.boolean(message.capabilities.replace) ||
      !reader.boolean(message.capabilities.withdraw) || !reader.boolean(message.capabilities.query) ||
      !reader.boolean(message.capabilities.read_only)) {
    why = "malformed backend capabilities";
    return false;
  }
  if (!reader.boolean(message.evidence_included)) {
    why = "malformed backend evidence flag";
    return false;
  }
  if (message.evidence_included) {
    if (!RouteKey::read(reader, limits.max_destination_chars, message.key)) {
      why = "malformed backend state route key";
      return false;
    }
    std::uint8_t presence = 0;
    if (!reader.u8(presence)) {
      why = "malformed backend presence";
      return false;
    }
    if (presence < static_cast<std::uint8_t>(BackendPresence::Unknown) ||
        presence > static_cast<std::uint8_t>(BackendPresence::Unavailable)) {
      why = "unknown backend presence value";
      return false;
    }
    message.presence = static_cast<BackendPresence>(presence);
    if (!reader.string(kMaxDiagnosticChars, message.evidence)) {
      why = "malformed backend evidence text";
      return false;
    }
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the backend state payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const FenceNotice& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  message.publisher.write(writer);
  message.worker_boot.write(writer);
  writer.u8(static_cast<std::uint8_t>(message.reason));
  message.epoch.write(writer);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, FenceNotice& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  FenceNotice message;
  if (!PublisherId::read(reader, message.publisher)) {
    why = "malformed fence notice publisher";
    return false;
  }
  if (!WorkerBootId::read(reader, message.worker_boot)) {
    why = "malformed fence notice worker boot";
    return false;
  }
  std::uint8_t reason = 0;
  if (!reader.u8(reason)) {
    why = "malformed fence notice reason";
    return false;
  }
  if (reason < static_cast<std::uint8_t>(FencingReason::SessionClosed) ||
      reason > static_cast<std::uint8_t>(FencingReason::EpochInvalidated)) {
    why = "unknown fencing reason value";
    return false;
  }
  message.reason = static_cast<FencingReason>(reason);
  if (!CoordinatorEpoch::read(reader, message.epoch)) {
    why = "malformed fence notice epoch";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the fence notice payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ErrorMessage& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ErrorMessage& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  ErrorMessage message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed error status";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the error payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ReconcileRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_context(writer, message.context);
  writer.boolean(message.all);
  write_optional_fixed(writer, message.route);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ReconcileRequest& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  ReconcileRequest message;
  if (!read_context(reader, message.context)) {
    why = "malformed reconcile authority context";
    return false;
  }
  if (!reader.boolean(message.all)) {
    why = "malformed reconcile selector";
    return false;
  }
  if (!read_optional_fixed(reader, message.route)) {
    why = "malformed reconcile route identity";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the reconcile payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ReconcileResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  writer.u8(static_cast<std::uint8_t>(message.classification));
  writer.u64(message.summary.checked);
  writer.u64(message.summary.matched);
  writer.u64(message.summary.missing);
  writer.u64(message.summary.diverged);
  writer.u64(message.summary.extra);
  writer.u64(message.summary.unavailable);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ReconcileResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  ReconcileResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed reconcile result status";
    return false;
  }
  std::uint8_t classification = 0;
  if (!reader.u8(classification)) {
    why = "malformed reconcile classification";
    return false;
  }
  if (classification < static_cast<std::uint8_t>(ObservationClass::Unknown) ||
      classification > static_cast<std::uint8_t>(ObservationClass::Unavailable)) {
    why = "unknown reconcile classification value";
    return false;
  }
  message.classification = static_cast<ObservationClass>(classification);
  std::uint64_t values[6] = {};
  for (std::uint64_t& value : values) {
    if (!reader.u64(value)) {
      why = "malformed reconcile summary";
      return false;
    }
  }
  message.summary.checked = static_cast<std::size_t>(values[0]);
  message.summary.matched = static_cast<std::size_t>(values[1]);
  message.summary.missing = static_cast<std::size_t>(values[2]);
  message.summary.diverged = static_cast<std::size_t>(values[3]);
  message.summary.extra = static_cast<std::size_t>(values[4]);
  message.summary.unavailable = static_cast<std::size_t>(values[5]);
  if (!reader.at_end()) {
    why = "trailing bytes in the reconcile result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const EpochResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  write_optional_counter(writer, message.epoch);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, EpochResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  EpochResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed epoch result status";
    return false;
  }
  if (!read_optional_counter(reader, message.epoch)) {
    why = "malformed epoch result epoch";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the epoch result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const StatisticsResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  message.epoch.write(writer);
  writer.raw(message.state_digest.bytes());
  writer.u64(message.route_count);
  writer.u64(message.installed_count);
  writer.u64(message.current_count);
  writer.u64(message.revalidation_required_count);
  writer.u64(message.withdrawn_count);
  writer.u64(message.retired_count);
  writer.u64(message.failed_count);
  writer.u64(message.superseded_count);
  writer.u64(message.publisher_count);
  writer.u64(message.path_dependency_count);
  writer.u64(message.outstanding_programming_count);
  writer.u64(message.revocation_count);
  writer.u64(message.routing_namespace_count);
  write_counter_fields(writer, message.counters);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, StatisticsResult& out, std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  StatisticsResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed statistics status";
    return false;
  }
  if (!CoordinatorEpoch::read(reader, message.epoch)) {
    why = "malformed statistics epoch";
    return false;
  }
  std::span<const std::uint8_t> digest_bytes;
  if (!reader.raw(16, digest_bytes)) {
    why = "malformed statistics digest";
    return false;
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    bytes[i] = digest_bytes[i];
  }
  message.state_digest = Digest128(bytes);
  std::uint64_t values[13] = {};
  for (std::uint64_t& value : values) {
    if (!reader.u64(value)) {
      why = "malformed statistics counters";
      return false;
    }
  }
  message.route_count = values[0];
  message.installed_count = values[1];
  message.current_count = values[2];
  message.revalidation_required_count = values[3];
  message.withdrawn_count = values[4];
  message.retired_count = values[5];
  message.failed_count = values[6];
  message.superseded_count = values[7];
  message.publisher_count = values[8];
  message.path_dependency_count = values[9];
  message.outstanding_programming_count = values[10];
  message.revocation_count = values[11];
  message.routing_namespace_count = values[12];
  if (!read_counter_fields(reader, message.counters)) {
    why = "malformed statistics counter block";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the statistics payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ApplyCompletionRequest& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_context(writer, message.context);
  message.completion.attempt.write(writer);
  writer.u8(static_cast<std::uint8_t>(message.completion.outcome));
  writer.string(message.completion.detail);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ApplyCompletionRequest& out,
            std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  ApplyCompletionRequest message;
  if (!read_context(reader, message.context)) {
    why = "malformed completion authority context";
    return false;
  }
  if (!ProgrammingAttemptId::read(reader, message.completion.attempt)) {
    why = "malformed completion attempt identity";
    return false;
  }
  std::uint8_t outcome = 0;
  if (!reader.u8(outcome)) {
    why = "malformed completion outcome";
    return false;
  }
  if (outcome < static_cast<std::uint8_t>(ProgrammingOutcome::Applied) ||
      outcome > static_cast<std::uint8_t>(ProgrammingOutcome::BackendUnavailable)) {
    why = "unknown programming outcome value";
    return false;
  }
  message.completion.outcome = static_cast<ProgrammingOutcome>(outcome);
  if (!reader.string(kMaxDiagnosticChars, message.completion.detail)) {
    why = "malformed completion detail";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the completion payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ApplyCompletionResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  writer.u8(static_cast<std::uint8_t>(message.disposition));
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ApplyCompletionResult& out,
            std::string& why) {
  (void)limits;
  ByteReader reader(payload);
  ApplyCompletionResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed completion result status";
    return false;
  }
  std::uint8_t disposition = 0;
  if (!reader.u8(disposition)) {
    why = "malformed completion disposition";
    return false;
  }
  if (disposition < static_cast<std::uint8_t>(CompletionDisposition::Applied) ||
      disposition > static_cast<std::uint8_t>(CompletionDisposition::LifecycleRejected)) {
    why = "unknown completion disposition value";
    return false;
  }
  message.disposition = static_cast<CompletionDisposition>(disposition);
  if (!reader.at_end()) {
    why = "trailing bytes in the completion result payload";
    return false;
  }
  out = std::move(message);
  return true;
}

std::vector<std::uint8_t> encode(const ExplainRouteResult& message, const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  write_status(writer, message.status, message.detail);
  writer.boolean(message.found);
  write_route_snapshot(writer, message.explanation.snapshot);
  writer.boolean(message.explanation.authoritative);
  writer.string(message.explanation.authority_reason);
  writer.string(message.explanation.currentness_reason);
  writer.string(message.explanation.lifecycle_reason);
  writer.string(message.explanation.path_authority_reason);
  writer.string(message.explanation.backend_reason);
  writer.string(message.explanation.reconciliation_reason);
  writer.string(message.explanation.supersession_reason);
  writer.string(message.explanation.retirement_reason);
  return writer.take();
}

bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ExplainRouteResult& out, std::string& why) {
  ByteReader reader(payload);
  ExplainRouteResult message;
  if (!read_status(reader, message.status, message.detail)) {
    why = "malformed explanation status";
    return false;
  }
  if (!reader.boolean(message.found)) {
    why = "malformed explanation flag";
    return false;
  }
  if (!read_route_snapshot(reader, limits, message.explanation.snapshot, why)) {
    return false;
  }
  if (!reader.boolean(message.explanation.authoritative)) {
    why = "malformed explanation authority flag";
    return false;
  }
  std::string* fields[8] = {&message.explanation.authority_reason,
                            &message.explanation.currentness_reason,
                            &message.explanation.lifecycle_reason,
                            &message.explanation.path_authority_reason,
                            &message.explanation.backend_reason,
                            &message.explanation.reconciliation_reason,
                            &message.explanation.supersession_reason,
                            &message.explanation.retirement_reason};
  for (std::string* field : fields) {
    if (!reader.string(kMaxDiagnosticChars * 8, *field)) {
      why = "malformed explanation text";
      return false;
    }
  }
  if (!reader.at_end()) {
    why = "trailing bytes in the explanation payload";
    return false;
  }
  out = std::move(message);
  return true;
}

}  // namespace routefabric
