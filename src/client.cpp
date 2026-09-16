#include "routefabric/client.hpp"

#include <string>
#include <vector>

#include "routefabric/hash.hpp"
#include "socket_internal.hpp"

namespace routefabric {

struct RouteFabricClient::Impl {
  explicit Impl(ClientConfig client_config)
      : config(std::move(client_config)),
        ids(config.id_seed != 0 ? config.id_seed : SeededIdSource::system_seed()) {}

  ClientConfig config;
  detail::SocketHandle socket = detail::kInvalidSocket;
  SeededIdSource ids;
  Limits limits;
  // Backend identity learned from the handshake; echoed in backend state
  // requests so both peers describe the same adapter.
  BackendId backend;
};

RouteFabricClient::RouteFabricClient(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
  context_.publisher = PublisherId();
  context_.worker_boot = WorkerBootId();
  context_.attempt = MutationAttemptId();
  context_.epoch = CoordinatorEpoch();
}

RouteFabricClient::~RouteFabricClient() { Close(); }

Status RouteFabricClient::Connect() {
  Impl& impl = *impl_;
  if (impl.socket != detail::kInvalidSocket) {
    return make_error(StatusCode::AlreadyExists, "the client is already connected");
  }
  if (impl.config.port == 0) {
    return make_error(StatusCode::InvalidArgument, "a coordinator port is required");
  }
  std::string why;
  bool ipv6 = false;
  if (!detail::create_tcp_socket(impl.config.host, impl.socket, ipv6, why)) {
    return make_error(StatusCode::Internal, why);
  }
  if (!detail::connect_socket(impl.socket, impl.config.host, impl.config.port, ipv6, why)) {
    detail::close_socket(impl.socket);
    impl.socket = detail::kInvalidSocket;
    return make_error(StatusCode::NotFound, why);
  }
  return ok_status();
}

void RouteFabricClient::Close() {
  Impl& impl = *impl_;
  if (impl.socket == detail::kInvalidSocket) {
    return;
  }
  detail::shutdown_socket(impl.socket);
  detail::close_socket(impl.socket);
  impl.socket = detail::kInvalidSocket;
}

bool RouteFabricClient::connected() const { return impl_->socket != detail::kInvalidSocket; }

MutationAttemptId RouteFabricClient::next_attempt() {
  context_.attempt = generate_id<MutationAttemptId>(impl_->ids);
  return context_.attempt;
}

Status RouteFabricClient::SendFrame(MessageId id, std::span<const std::uint8_t> payload) {
  Impl& impl = *impl_;
  if (impl.socket == detail::kInvalidSocket) {
    return make_error(StatusCode::NotFound, "the client is not connected");
  }
  if (payload.size() > impl.config.limits.max_frame_bytes) {
    return make_error(StatusCode::LimitExceeded, "the frame payload exceeds the configured bound");
  }
  const std::vector<std::uint8_t> frame = encode_frame(id, payload);
  std::string why;
  if (!detail::send_all(impl.socket, frame.data(), frame.size(), why)) {
    return make_error(StatusCode::ProtocolViolation, why);
  }
  return ok_status();
}

Status RouteFabricClient::SendRawBytes(std::span<const std::uint8_t> frame) {
  Impl& impl = *impl_;
  if (impl.socket == detail::kInvalidSocket) {
    return make_error(StatusCode::NotFound, "the client is not connected");
  }
  std::string why;
  if (!detail::send_all(impl.socket, frame.data(), frame.size(), why)) {
    return make_error(StatusCode::ProtocolViolation, why);
  }
  return ok_status();
}

Expected<DecodedFrame> RouteFabricClient::ReceiveFrame() {
  Impl& impl = *impl_;
  if (impl.socket == detail::kInvalidSocket) {
    return make_error<DecodedFrame>(StatusCode::NotFound, "the client is not connected");
  }
  std::uint8_t header[kFrameHeaderBytes] = {};
  std::string why;
  if (!detail::recv_exact(impl.socket, header, kFrameHeaderBytes, why)) {
    return make_error<DecodedFrame>(StatusCode::ProtocolViolation, why);
  }
  std::size_t total = 0;
  const FrameStatus header_status =
      frame_header_status(std::span<const std::uint8_t>(header, kFrameHeaderBytes), impl.config.limits, total, why);
  if (header_status != FrameStatus::Complete) {
    return make_error<DecodedFrame>(StatusCode::ProtocolViolation, why);
  }
  std::vector<std::uint8_t> frame(total);
  for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
    frame[i] = header[i];
  }
  if (total > kFrameHeaderBytes &&
      !detail::recv_exact(impl.socket, frame.data() + kFrameHeaderBytes, total - kFrameHeaderBytes, why)) {
    return make_error<DecodedFrame>(StatusCode::ProtocolViolation, why);
  }
  DecodedFrame decoded;
  const FrameStatus status = decode_frame(frame, impl.config.limits, decoded, why);
  if (status != FrameStatus::Complete) {
    return make_error<DecodedFrame>(StatusCode::IntegrityFailure, why);
  }
  return decoded;
}

namespace {

Expected<DecodedFrame> expect_frame(RouteFabricClient& client, MessageId expected, const Limits& limits) {
  Expected<DecodedFrame> frame = client.ReceiveFrame();
  if (!frame) {
    return frame;
  }
  if (frame.value().id == MessageId::Error) {
    ErrorMessage message;
    std::string why;
    if (decode(frame.value().payload, limits, message, why)) {
      return make_error<DecodedFrame>(message.status, message.detail);
    }
    return make_error<DecodedFrame>(StatusCode::ProtocolViolation, "malformed error response");
  }
  if (frame.value().id != expected) {
    return make_error<DecodedFrame>(StatusCode::ProtocolViolation,
                                    std::string("unexpected response ") + to_string(frame.value().id));
  }
  return frame;
}

// Rejects an unusable authority context locally instead of emitting a frame the
// peer cannot decode.
Status validate_context(const RequestContext& context) {
  if (!context.publisher.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "the client has no publisher identity");
  }
  if (!context.worker_boot.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "the client has no worker boot identity");
  }
  if (!context.epoch.is_valid()) {
    return make_error(StatusCode::InvalidArgument,
                      "the client has no coordinator epoch; complete a hello handshake or set one explicitly");
  }
  return ok_status();
}

}  // namespace

Expected<HelloResult> RouteFabricClient::Hello(std::string_view client_name) {
  Impl& impl = *impl_;
  HelloRequest request;
  request.wire_version = kWireProtocolVersion;
  request.client_name = std::string(client_name);
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::Hello, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::HelloResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  HelloResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<HelloResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status == StatusCode::Ok) {
    context_.epoch = result.epoch;
    impl.backend = result.backend;
  }
  return result;
}

Expected<RegisterPublisherResult> RouteFabricClient::RegisterPublisher(const PublisherScope& scope) {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  if (!context_.attempt.is_valid()) {
    return make_error<RegisterPublisherResult>(
        StatusCode::InvalidArgument, "the client has no mutation attempt identity; call next_attempt()");
  }
  RegisterPublisherRequest request;
  request.context = context_;
  request.scope = scope;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::RegisterPublisher, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::RegisterPublisherResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  RegisterPublisherResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<RegisterPublisherResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status == StatusCode::Ok && result.epoch.is_valid()) {
    context_.epoch = result.epoch;
  }
  return result;
}

Expected<PublishRouteResult> RouteFabricClient::PublishRoute(const RouteKey& key, const RouteBinding& binding,
                                                             const PolicyGeneration& policy_generation,
                                                             const RouteGeneration& expected_generation,
                                                             const RouteId& lineage, std::string reason) {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  if (!context_.attempt.is_valid()) {
    return make_error<PublishRouteResult>(StatusCode::InvalidArgument,
                                          "the client has no mutation attempt identity; call next_attempt()");
  }
  PublishRouteRequest request;
  request.context = context_;
  request.key = key;
  request.binding = binding;
  request.policy_generation = policy_generation;
  request.expected_generation = expected_generation;
  request.lineage = lineage;
  request.reason = std::move(reason);
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::PublishRoute, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::PublishRouteResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  PublishRouteResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<PublishRouteResult>(StatusCode::ProtocolViolation, why);
  }
  return result;
}

namespace {

Expected<RouteMutationResult> send_mutation(RouteFabricClient& client, MessageId request_id, MessageId response_id,
                                            const RequestContext& context, const RouteId& route,
                                            const RouteGeneration& expected_generation, std::string reason,
                                            const Limits& limits) {
  const Status context_status = validate_context(context);
  if (!context_status) {
    return context_status.error();
  }
  if (!context.attempt.is_valid()) {
    return make_error<RouteMutationResult>(StatusCode::InvalidArgument,
                                           "no mutation attempt identity; call next_attempt()");
  }
  RouteMutationRequest request;
  request.context = context;
  request.route = route;
  request.expected_generation = expected_generation;
  request.reason = std::move(reason);
  const std::vector<std::uint8_t> payload = encode(request, limits);
  Status status = client.SendFrame(request_id, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(client, response_id, limits);
  if (!frame) {
    return frame.error();
  }
  RouteMutationResult result;
  std::string why;
  if (!decode(frame.value().payload, limits, result, why)) {
    return make_error<RouteMutationResult>(StatusCode::ProtocolViolation, why);
  }
  return result;
}

}  // namespace

Expected<RouteMutationResult> RouteFabricClient::WithdrawRoute(const RouteId& route,
                                                              const RouteGeneration& expected_generation,
                                                              std::string reason) {
  return send_mutation(*this, MessageId::WithdrawRoute, MessageId::WithdrawRouteResult, context_, route,
                       expected_generation, std::move(reason), impl_->config.limits);
}

Expected<RouteMutationResult> RouteFabricClient::RevalidateRoute(const RouteId& route, std::string reason) {
  return send_mutation(*this, MessageId::RevalidateRoute, MessageId::RevalidateRouteResult, context_, route,
                       RouteGeneration(), std::move(reason), impl_->config.limits);
}

Expected<RouteMutationResult> RouteFabricClient::RetireRoute(const RouteId& route, std::string reason) {
  return send_mutation(*this, MessageId::RetireRoute, MessageId::RetireRouteResult, context_, route,
                       RouteGeneration(), std::move(reason), impl_->config.limits);
}

Expected<RouteMutationResult> RouteFabricClient::RevokeRoute(const RouteId& route, std::string reason) {
  return send_mutation(*this, MessageId::RevokeRoute, MessageId::RevokeRouteResult, context_, route,
                       RouteGeneration(), std::move(reason), impl_->config.limits);
}

namespace {

Expected<QueryRouteResult> send_query(RouteFabricClient& client, const QueryRouteRequest& request, const Limits& limits) {
  const std::vector<std::uint8_t> payload = encode(request, limits);
  Status status = client.SendFrame(MessageId::QueryRoute, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(client, MessageId::QueryRouteResult, limits);
  if (!frame) {
    return frame.error();
  }
  QueryRouteResult result;
  std::string why;
  if (!decode(frame.value().payload, limits, result, why)) {
    return make_error<QueryRouteResult>(StatusCode::ProtocolViolation, why);
  }
  return result;
}

}  // namespace

Expected<RouteSnapshot> RouteFabricClient::QueryRouteByKey(const RouteKey& key) {
  QueryRouteRequest request;
  request.by_key = true;
  request.key = key;
  Expected<QueryRouteResult> result = send_query(*this, request, impl_->config.limits);
  if (!result) {
    return result.error();
  }
  if (result.value().status != StatusCode::Ok) {
    return make_error<RouteSnapshot>(result.value().status, result.value().detail);
  }
  if (!result.value().found) {
    return make_error<RouteSnapshot>(StatusCode::NotFound, "no route for that key");
  }
  return result.value().snapshot;
}

Expected<RouteSnapshot> RouteFabricClient::QueryRouteById(const RouteId& route) {
  QueryRouteRequest request;
  request.by_key = false;
  request.route = route;
  Expected<QueryRouteResult> result = send_query(*this, request, impl_->config.limits);
  if (!result) {
    return result.error();
  }
  if (result.value().status != StatusCode::Ok) {
    return make_error<RouteSnapshot>(result.value().status, result.value().detail);
  }
  if (!result.value().found) {
    return make_error<RouteSnapshot>(StatusCode::NotFound, "no route with that identity");
  }
  return result.value().snapshot;
}

Expected<SnapshotResponse> RouteFabricClient::Snapshot() {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  SnapshotRequest request;
  request.epoch = context_.epoch;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::SnapshotRequest, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::SnapshotResponse, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  SnapshotResponse response;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, response, why)) {
    return make_error<SnapshotResponse>(StatusCode::ProtocolViolation, why);
  }
  if (response.status != StatusCode::Ok) {
    return make_error<SnapshotResponse>(response.status, response.detail);
  }
  return response;
}

namespace {

Expected<ExplainRouteResult> send_explain(RouteFabricClient& client, const QueryRouteRequest& request,
                                          const Limits& limits) {
  const std::vector<std::uint8_t> payload = encode(request, limits);
  Status status = client.SendFrame(MessageId::ExplainRoute, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(client, MessageId::ExplainRouteResult, limits);
  if (!frame) {
    return frame.error();
  }
  ExplainRouteResult result;
  std::string why;
  if (!decode(frame.value().payload, limits, result, why)) {
    return make_error<ExplainRouteResult>(StatusCode::ProtocolViolation, why);
  }
  return result;
}

}  // namespace

Expected<RouteExplanation> RouteFabricClient::ExplainRouteByKey(const RouteKey& key) {
  QueryRouteRequest request;
  request.by_key = true;
  request.key = key;
  Expected<ExplainRouteResult> result = send_explain(*this, request, impl_->config.limits);
  if (!result) {
    return result.error();
  }
  if (result.value().status != StatusCode::Ok) {
    return make_error<RouteExplanation>(result.value().status, result.value().detail);
  }
  if (!result.value().found) {
    return make_error<RouteExplanation>(StatusCode::NotFound, "no route for that key");
  }
  return result.value().explanation;
}

Expected<RouteExplanation> RouteFabricClient::ExplainRouteById(const RouteId& route) {
  QueryRouteRequest request;
  request.by_key = false;
  request.route = route;
  Expected<ExplainRouteResult> result = send_explain(*this, request, impl_->config.limits);
  if (!result) {
    return result.error();
  }
  if (result.value().status != StatusCode::Ok) {
    return make_error<RouteExplanation>(result.value().status, result.value().detail);
  }
  if (!result.value().found) {
    return make_error<RouteExplanation>(StatusCode::NotFound, "no route with that identity");
  }
  return result.value().explanation;
}

Expected<StatisticsResult> RouteFabricClient::Statistics() {
  Impl& impl = *impl_;
  const std::vector<std::uint8_t> payload;
  Status status = SendFrame(MessageId::StatisticsRequest, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::StatisticsResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  StatisticsResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<StatisticsResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<StatisticsResult>(result.status, result.detail);
  }
  return result;
}

Expected<CoordinatorEpoch> RouteFabricClient::Epoch() {
  Impl& impl = *impl_;
  const std::vector<std::uint8_t> payload;
  Status status = SendFrame(MessageId::EpochRequest, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::EpochResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  EpochResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<CoordinatorEpoch>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<CoordinatorEpoch>(result.status, result.detail);
  }
  return result.epoch;
}

Expected<ReconcileResult> RouteFabricClient::ReconcileAll() {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  ReconcileRequest request;
  request.context = context_;
  request.all = true;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::ReconcileRequest, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::ReconcileResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  ReconcileResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<ReconcileResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<ReconcileResult>(result.status, result.detail);
  }
  return result;
}

Expected<ReconcileResult> RouteFabricClient::ReconcileRoute(const RouteId& route) {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  ReconcileRequest request;
  request.context = context_;
  request.all = false;
  request.route = route;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::ReconcileRequest, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::ReconcileResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  ReconcileResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<ReconcileResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<ReconcileResult>(result.status, result.detail);
  }
  return result;
}

Expected<ApplyCompletionResult> RouteFabricClient::ApplyCompletion(const ProgrammingCompletion& completion) {
  Impl& impl = *impl_;
  const Status context_status = validate_context(context_);
  if (!context_status) {
    return context_status.error();
  }
  ApplyCompletionRequest request;
  request.context = context_;
  request.completion = completion;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::ApplyCompletion, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::ApplyCompletionResult, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  ApplyCompletionResult result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<ApplyCompletionResult>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<ApplyCompletionResult>(result.status, result.detail);
  }
  return result;
}

Expected<BackendStateMessage> RouteFabricClient::BackendState() {
  Impl& impl = *impl_;
  if (!impl.backend.is_valid()) {
    return make_error<BackendStateMessage>(StatusCode::NotOpen,
                                           "the client has not completed a handshake with a coordinator");
  }
  BackendStateMessage request;
  request.backend = impl.backend;
  request.evidence_included = false;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::BackendState, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::BackendState, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  BackendStateMessage result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<BackendStateMessage>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<BackendStateMessage>(result.status, result.detail);
  }
  return result;
}

Expected<BackendStateMessage> RouteFabricClient::BackendStateFor(const RouteKey& key) {
  Impl& impl = *impl_;
  if (!impl.backend.is_valid()) {
    return make_error<BackendStateMessage>(StatusCode::NotOpen,
                                           "the client has not completed a handshake with a coordinator");
  }
  BackendStateMessage request;
  request.backend = impl.backend;
  request.evidence_included = true;
  request.key = key;
  const std::vector<std::uint8_t> payload = encode(request, impl.config.limits);
  Status status = SendFrame(MessageId::BackendState, payload);
  if (!status) {
    return status.error();
  }
  Expected<DecodedFrame> frame = expect_frame(*this, MessageId::BackendState, impl.config.limits);
  if (!frame) {
    return frame.error();
  }
  BackendStateMessage result;
  std::string why;
  if (!decode(frame.value().payload, impl.config.limits, result, why)) {
    return make_error<BackendStateMessage>(StatusCode::ProtocolViolation, why);
  }
  if (result.status != StatusCode::Ok) {
    return make_error<BackendStateMessage>(result.status, result.detail);
  }
  return result;
}

}  // namespace routefabric
