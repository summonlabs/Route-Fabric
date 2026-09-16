#ifndef ROUTEFABRIC_WIRE_HPP
#define ROUTEFABRIC_WIRE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/error.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/persistence.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/snapshot.hpp"
#include "routefabric/version.hpp"

namespace routefabric {

// Explicit, stable wire message identifiers. The numeric values are part of the
// wire protocol and must never change once released.
enum class MessageId : std::uint16_t {
  Hello = 1,
  HelloResult = 2,
  RegisterPublisher = 3,
  RegisterPublisherResult = 4,
  PublishRoute = 5,
  PublishRouteResult = 6,
  WithdrawRoute = 7,
  WithdrawRouteResult = 8,
  RevalidateRoute = 9,
  RevalidateRouteResult = 10,
  RetireRoute = 11,
  RetireRouteResult = 12,
  RevokeRoute = 13,
  RevokeRouteResult = 14,
  QueryRoute = 15,
  QueryRouteResult = 16,
  SnapshotRequest = 17,
  SnapshotResponse = 18,
  BackendState = 19,
  FenceNotice = 20,
  Error = 21,
  ReconcileRequest = 22,
  ReconcileResult = 23,
  EpochRequest = 24,
  EpochResult = 25,
  StatisticsRequest = 26,
  StatisticsResult = 27,
  ApplyCompletion = 28,
  ApplyCompletionResult = 29,
  ExplainRoute = 30,
  ExplainRouteResult = 31,
};

const char* to_string(MessageId id) noexcept;
bool is_message_id_value(std::uint16_t raw) noexcept;

inline constexpr std::size_t kFrameHeaderBytes = 16;

// A frame is: magic (u32), wire version (u16), message id (u16), payload length
// (u32), integrity (u32), payload. The integrity field is CRC-32C over the first
// twelve header bytes plus the payload, so it covers every semantic header field
// and the complete payload. The transport is plain TCP; integrity checking is
// non-cryptographic and provides no authentication.
std::vector<std::uint8_t> encode_frame(MessageId id, std::span<const std::uint8_t> payload);

enum class FrameStatus : std::uint8_t {
  Complete = 1,
  Incomplete = 2,
  InvalidMagic = 3,
  InvalidVersion = 4,
  InvalidMessageId = 5,
  OversizedPayload = 6,
  TrailingBytes = 7,
  IntegrityFailure = 8,
};

const char* to_string(FrameStatus status) noexcept;
bool is_complete(FrameStatus status) noexcept;
bool is_invalid(FrameStatus status) noexcept;

struct DecodedFrame {
  MessageId id = MessageId::Error;
  std::uint16_t wire_version = 0;
  std::vector<std::uint8_t> payload;
};

// Decodes exactly one frame. Returns Incomplete when more bytes are required and
// a specific failure status when the frame violates the protocol.
FrameStatus decode_frame(std::span<const std::uint8_t> buffer, const Limits& limits, DecodedFrame& out,
                         std::string& why);

// Validates a frame header and reports the total frame size. Returns Incomplete
// when the header itself is not yet complete.
FrameStatus frame_header_status(std::span<const std::uint8_t> buffer, const Limits& limits, std::size_t& total,
                                std::string& why);

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

struct RequestContext {
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
};

struct HelloRequest {
  std::uint16_t wire_version = 0;
  std::string client_name;
};

struct HelloResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  std::uint16_t wire_version = 0;
  CoordinatorEpoch epoch;
  FabricId fabric;
  BackendId backend;
  bool backend_read_only = false;
};

struct RegisterPublisherRequest {
  RequestContext context;
  PublisherScope scope;
};

struct RegisterPublisherResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  CoordinatorEpoch epoch;
};

struct PublishRouteRequest {
  RequestContext context;
  RouteKey key;
  RouteBinding binding;
  PolicyGeneration policy_generation;
  RouteGeneration expected_generation;
  RouteId lineage;
  std::string reason;
};

struct PublishRouteResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  RouteId route;
  RouteGeneration generation;
  RouteAuthorityGeneration authority_generation;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  AppliedClassification applied = AppliedClassification::Unknown;
  RouteCurrentness currentness = RouteCurrentness::NotInstalled;
  bool idempotent = false;
  bool replaced = false;
};

struct RouteMutationRequest {
  RequestContext context;
  RouteId route;
  RouteGeneration expected_generation;
  std::string reason;
};

struct RouteMutationResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  RouteId route;
  RouteGeneration generation;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  AppliedClassification applied = AppliedClassification::Unknown;
  bool already_withdrawn = false;
  bool withdrawal_in_flight = false;
};

struct QueryRouteRequest {
  bool by_key = true;
  RouteKey key;
  RouteId route;
};

struct QueryRouteResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  bool found = false;
  RouteSnapshot snapshot;
};

struct SnapshotRequest {
  CoordinatorEpoch epoch;
};

struct SnapshotResponse {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  CoordinatorEpoch epoch;
  RouteSnapshotId snapshot_id;
  Digest128 digest;
  std::vector<RouteSnapshot> routes;
};

struct BackendStateMessage {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  BackendId backend;
  BackendCapabilities capabilities;
  bool evidence_included = false;
  RouteKey key;
  BackendPresence presence = BackendPresence::Unknown;
  std::string evidence;
};

struct FenceNotice {
  PublisherId publisher;
  WorkerBootId worker_boot;
  FencingReason reason = FencingReason::SessionClosed;
  CoordinatorEpoch epoch;
};

struct ErrorMessage {
  StatusCode status = StatusCode::Internal;
  std::string detail;
};

struct ReconcileRequest {
  RequestContext context;
  bool all = false;
  RouteId route;
};

struct ReconcileResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  ObservationClass classification = ObservationClass::Unknown;
  ReconciliationSummary summary;
};

struct EpochResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  CoordinatorEpoch epoch;
};

struct StatisticsResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  CoordinatorEpoch epoch;
  Digest128 state_digest;
  std::uint64_t route_count = 0;
  std::uint64_t installed_count = 0;
  std::uint64_t current_count = 0;
  std::uint64_t revalidation_required_count = 0;
  std::uint64_t withdrawn_count = 0;
  std::uint64_t retired_count = 0;
  std::uint64_t failed_count = 0;
  std::uint64_t superseded_count = 0;
  std::uint64_t publisher_count = 0;
  std::uint64_t path_dependency_count = 0;
  std::uint64_t outstanding_programming_count = 0;
  std::uint64_t revocation_count = 0;
  std::uint64_t routing_namespace_count = 0;
  RouteCounters counters;
};

struct ApplyCompletionRequest {
  RequestContext context;
  ProgrammingCompletion completion;
};

struct ExplainRouteResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  bool found = false;
  RouteExplanation explanation;
};

struct ApplyCompletionResult {
  StatusCode status = StatusCode::Ok;
  std::string detail;
  CompletionDisposition disposition = CompletionDisposition::UnknownAttempt;
};

// Codecs. Every decoder validates bounds, rejects unknown enum values and
// requires the payload to be consumed exactly.
std::vector<std::uint8_t> encode(const HelloRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, HelloRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const HelloResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, HelloResult& out, std::string& why);

std::vector<std::uint8_t> encode(const RegisterPublisherRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RegisterPublisherRequest& out,
            std::string& why);
std::vector<std::uint8_t> encode(const RegisterPublisherResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RegisterPublisherResult& out,
            std::string& why);

std::vector<std::uint8_t> encode(const PublishRouteRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, PublishRouteRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const PublishRouteResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, PublishRouteResult& out, std::string& why);

std::vector<std::uint8_t> encode(const RouteMutationRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RouteMutationRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const RouteMutationResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, RouteMutationResult& out, std::string& why);

std::vector<std::uint8_t> encode(const QueryRouteRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, QueryRouteRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const QueryRouteResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, QueryRouteResult& out, std::string& why);

std::vector<std::uint8_t> encode(const SnapshotRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, SnapshotRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const SnapshotResponse& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, SnapshotResponse& out, std::string& why);

std::vector<std::uint8_t> encode(const BackendStateMessage& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, BackendStateMessage& out, std::string& why);

std::vector<std::uint8_t> encode(const FenceNotice& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, FenceNotice& out, std::string& why);

std::vector<std::uint8_t> encode(const ErrorMessage& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ErrorMessage& out, std::string& why);

std::vector<std::uint8_t> encode(const ReconcileRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ReconcileRequest& out, std::string& why);
std::vector<std::uint8_t> encode(const ReconcileResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ReconcileResult& out, std::string& why);

std::vector<std::uint8_t> encode(const EpochResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, EpochResult& out, std::string& why);

std::vector<std::uint8_t> encode(const StatisticsResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, StatisticsResult& out, std::string& why);

std::vector<std::uint8_t> encode(const ApplyCompletionRequest& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ApplyCompletionRequest& out,
            std::string& why);
std::vector<std::uint8_t> encode(const ApplyCompletionResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ApplyCompletionResult& out,
            std::string& why);

std::vector<std::uint8_t> encode(const ExplainRouteResult& message, const Limits& limits);
bool decode(std::span<const std::uint8_t> payload, const Limits& limits, ExplainRouteResult& out, std::string& why);

// Scope and snapshot codecs are shared with the persistence layer and are
// exposed for tests.
void write_publisher_scope(ByteWriter& writer, const PublisherScope& scope);
bool read_publisher_scope(ByteReader& reader, const Limits& limits, PublisherScope& out);
void write_route_snapshot(ByteWriter& writer, const RouteSnapshot& snapshot);
bool read_route_snapshot(ByteReader& reader, const Limits& limits, RouteSnapshot& out, std::string& why);

}  // namespace routefabric

#endif  // ROUTEFABRIC_WIRE_HPP
