#include "routefabric/server.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "routefabric/wire.hpp"
#include "socket_internal.hpp"

#include <algorithm>

namespace routefabric {

struct RouteFabricServer::Impl {
  struct Session {
    detail::SocketHandle socket = detail::kInvalidSocket;
    std::shared_ptr<detail::SocketWaiter> waiter;
    PublisherId publisher;
    WorkerBootId worker_boot;
    bool registered = false;
  };

  Impl(ServerConfig server_config, RouteFabricRuntime* route_runtime, IRouteProgrammingBackend* programming_backend)
      : config(std::move(server_config)), runtime(route_runtime), backend(programming_backend) {}

  ServerConfig config;
  RouteFabricRuntime* runtime = nullptr;
  IRouteProgrammingBackend* backend = nullptr;

  detail::SocketHandle listener = detail::kInvalidSocket;
  std::thread accept_thread;
  mutable std::mutex mutex;
  std::condition_variable session_finished;
  std::vector<std::shared_ptr<Session>> sessions;
  std::size_t active_sessions = 0;
  std::atomic<bool> stopping{false};
  std::atomic<bool> running{false};
  std::uint16_t port = 0;
  ServerCounters counters;

  void bump(std::uint64_t& counter) {
    std::lock_guard<std::mutex> lock(mutex);
    ++counter;
  }

  bool send_payload(const std::shared_ptr<Session>& session, MessageId id, std::span<const std::uint8_t> payload) {
    const std::vector<std::uint8_t> frame = encode_frame(id, payload);
    std::string why;
    return detail::send_all(session->socket, frame.data(), frame.size(), why);
  }

  bool send_error(const std::shared_ptr<Session>& session, StatusCode code, const std::string& detail) {
    ErrorMessage message;
    message.status = code;
    message.detail = detail;
    const std::vector<std::uint8_t> payload = encode(message, config.limits);
    return send_payload(session, MessageId::Error, payload);
  }

  // Returns false when the session must be closed because the peer violated the
  // protocol. A peer that emits a malformed payload is not trusted to continue.
  bool handle(const std::shared_ptr<Session>& session, const DecodedFrame& frame) {
    bump(counters.requests_handled);
    switch (frame.id) {
      case MessageId::Hello: {
        HelloRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        HelloResult result;
        result.status = StatusCode::Ok;
        result.detail = "route fabric coordinator session established";
        result.wire_version = kWireProtocolVersion;
        result.epoch = runtime->epoch();
        result.fabric = runtime->fabric();
        result.backend = backend->id();
        result.backend_read_only = backend->capabilities().read_only;
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::HelloResult, payload);
        return true;
      }
      case MessageId::RegisterPublisher: {
        RegisterPublisherRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        RegisterPublisherResult result;
        const Expected<PublisherRegistration> registered = runtime->RegisterPublisher(
            request.context.publisher, request.context.worker_boot, request.scope);
        if (registered) {
          result.status = StatusCode::Ok;
          result.detail = "publisher registered";
          result.epoch = registered.value().epoch;
          session->publisher = request.context.publisher;
          session->worker_boot = request.context.worker_boot;
          session->registered = true;
        } else {
          result.status = registered.error().code();
          result.detail = registered.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::RegisterPublisherResult, payload);
        return true;
      }
      case MessageId::PublishRoute: {
        PublishRouteRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        PublishRequest publish;
        publish.publisher = request.context.publisher;
        publish.worker_boot = request.context.worker_boot;
        publish.attempt = request.context.attempt;
        publish.key = request.key;
        publish.binding = request.binding;
        publish.policy_generation = request.policy_generation;
        publish.expected_generation = request.expected_generation;
        publish.lineage = request.lineage;
        publish.reason = request.reason;
        const Expected<PublishResult> published = runtime->PublishRoute(publish);
        PublishRouteResult result;
        if (published) {
          result.status = StatusCode::Ok;
          result.route = published.value().route;
          result.generation = published.value().generation;
          result.authority_generation = published.value().authority_generation;
          result.lifecycle = published.value().lifecycle;
          result.applied = published.value().applied;
          result.currentness = published.value().currentness;
          result.idempotent = published.value().idempotent;
          result.replaced = published.value().replaced;
        } else {
          result.status = published.error().code();
          result.detail = published.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::PublishRouteResult, payload);
        return true;
      }
      case MessageId::WithdrawRoute:
      case MessageId::RevalidateRoute:
      case MessageId::RetireRoute:
      case MessageId::RevokeRoute: {
        RouteMutationRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        Expected<WithdrawOutcome> outcome =
            make_error<WithdrawOutcome>(StatusCode::Internal, "unhandled mutation");
        if (frame.id == MessageId::WithdrawRoute) {
          WithdrawRequest withdraw;
          withdraw.publisher = request.context.publisher;
          withdraw.worker_boot = request.context.worker_boot;
          withdraw.attempt = request.context.attempt;
          withdraw.route = request.route;
          withdraw.expected_generation = request.expected_generation;
          withdraw.reason = request.reason;
          outcome = runtime->WithdrawRoute(withdraw);
        } else if (frame.id == MessageId::RevalidateRoute) {
          RevalidateRequest revalidate;
          revalidate.publisher = request.context.publisher;
          revalidate.worker_boot = request.context.worker_boot;
          revalidate.attempt = request.context.attempt;
          revalidate.route = request.route;
          revalidate.reason = request.reason;
          outcome = runtime->RevalidateRoute(revalidate);
        } else if (frame.id == MessageId::RetireRoute) {
          RetireRequest retire;
          retire.publisher = request.context.publisher;
          retire.worker_boot = request.context.worker_boot;
          retire.attempt = request.context.attempt;
          retire.route = request.route;
          retire.reason = request.reason;
          outcome = runtime->RetireRoute(retire);
        } else {
          RevokeRequest revoke;
          revoke.publisher = request.context.publisher;
          revoke.worker_boot = request.context.worker_boot;
          revoke.attempt = request.context.attempt;
          revoke.route = request.route;
          revoke.reason = request.reason;
          outcome = runtime->RevokeRoute(revoke);
        }
        RouteMutationResult result;
        if (outcome) {
          result.status = StatusCode::Ok;
          result.route = outcome.value().route;
          result.generation = outcome.value().generation;
          result.lifecycle = outcome.value().lifecycle;
          result.applied = outcome.value().applied;
          result.already_withdrawn = outcome.value().already_withdrawn;
          result.withdrawal_in_flight = outcome.value().withdrawal_in_flight;
        } else {
          result.status = outcome.error().code();
          result.detail = outcome.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        MessageId response = MessageId::WithdrawRouteResult;
        if (frame.id == MessageId::RevalidateRoute) {
          response = MessageId::RevalidateRouteResult;
        } else if (frame.id == MessageId::RetireRoute) {
          response = MessageId::RetireRouteResult;
        } else if (frame.id == MessageId::RevokeRoute) {
          response = MessageId::RevokeRouteResult;
        }
        send_payload(session, response, payload);
        return true;
      }
      case MessageId::QueryRoute: {
        QueryRouteRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        QueryRouteResult result;
        const Expected<RouteSnapshot> snapshot =
            request.by_key ? runtime->QueryRoute(request.key) : runtime->QueryRouteById(request.route);
        if (snapshot) {
          result.status = StatusCode::Ok;
          result.found = true;
          result.snapshot = snapshot.value();
        } else if (snapshot.error().code() == StatusCode::NotFound) {
          result.status = StatusCode::Ok;
          result.found = false;
          result.detail = "no route";
        } else {
          result.status = snapshot.error().code();
          result.detail = snapshot.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::QueryRouteResult, payload);
        return true;
      }
      case MessageId::ExplainRoute: {
        QueryRouteRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        ExplainRouteResult result;
        const Expected<RouteExplanation> explanation =
            request.by_key ? runtime->ExplainRoute(request.key) : runtime->ExplainRouteById(request.route);
        if (explanation) {
          result.status = StatusCode::Ok;
          result.found = true;
          result.explanation = explanation.value();
        } else if (explanation.error().code() == StatusCode::NotFound) {
          result.status = StatusCode::Ok;
          result.found = false;
          result.detail = "no route";
        } else {
          result.status = explanation.error().code();
          result.detail = explanation.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::ExplainRouteResult, payload);
        return true;
      }
      case MessageId::SnapshotRequest: {
        SnapshotRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        SnapshotResponse result;
        const Expected<RouteSnapshotSet> snapshot = runtime->Snapshot();
        if (snapshot) {
          result.status = StatusCode::Ok;
          result.epoch = snapshot.value().epoch;
          result.snapshot_id = snapshot.value().id;
          result.digest = snapshot.value().digest;
          result.routes = snapshot.value().routes;
        } else {
          result.status = snapshot.error().code();
          result.detail = snapshot.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::SnapshotResponse, payload);
        return true;
      }
      case MessageId::StatisticsRequest: {
        StatisticsResult result;
        const RouteStatistics statistics = runtime->Statistics();
        result.status = StatusCode::Ok;
        result.epoch = statistics.epoch;
        result.state_digest = statistics.state_digest;
        result.route_count = statistics.route_count;
        result.installed_count = statistics.installed_count;
        result.current_count = statistics.current_count;
        result.revalidation_required_count = statistics.revalidation_required_count;
        result.withdrawn_count = statistics.withdrawn_count;
        result.retired_count = statistics.retired_count;
        result.failed_count = statistics.failed_count;
        result.superseded_count = statistics.superseded_count;
        result.publisher_count = statistics.publisher_count;
        result.path_dependency_count = statistics.path_dependency_count;
        result.outstanding_programming_count = statistics.outstanding_programming_count;
        result.revocation_count = statistics.revocation_count;
        result.routing_namespace_count = statistics.routing_namespace_count;
        result.counters = statistics.counters;
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::StatisticsResult, payload);
        return true;
      }
      case MessageId::EpochRequest: {
        EpochResult result;
        result.status = StatusCode::Ok;
        result.epoch = runtime->epoch();
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::EpochResult, payload);
        return true;
      }
      case MessageId::ReconcileRequest: {
        ReconcileRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        ReconcileResult result;
        if (request.all) {
          const Expected<ReconciliationSummary> summary = runtime->ReconcileAll();
          if (summary) {
            result.status = StatusCode::Ok;
            result.summary = summary.value();
          } else {
            result.status = summary.error().code();
            result.detail = summary.error().detail();
          }
        } else {
          const Expected<ObservationClass> classification = runtime->ReconcileRoute(request.route);
          if (classification) {
            result.status = StatusCode::Ok;
            result.classification = classification.value();
            result.summary.checked = 1;
          } else {
            result.status = classification.error().code();
            result.detail = classification.error().detail();
          }
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::ReconcileResult, payload);
        return true;
      }
      case MessageId::ApplyCompletion: {
        ApplyCompletionRequest request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        ApplyCompletionResult result;
        const Expected<CompletionDisposition> disposition =
            runtime->ApplyProgrammingCompletion(request.completion);
        if (disposition) {
          result.status = StatusCode::Ok;
          result.disposition = disposition.value();
        } else {
          result.status = disposition.error().code();
          result.detail = disposition.error().detail();
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::ApplyCompletionResult, payload);
        return true;
      }
      case MessageId::BackendState: {
        BackendStateMessage request;
        std::string why;
        if (!decode(frame.payload, config.limits, request, why)) {
          bump(counters.protocol_errors);
          send_error(session, StatusCode::ProtocolViolation, why);
          return false;
        }
        BackendStateMessage result;
        result.status = StatusCode::Ok;
        result.backend = backend->id();
        result.capabilities = backend->capabilities();
        if (request.evidence_included) {
          const BackendQueryResult query = backend->QueryRoute(request.key);
          result.evidence_included = true;
          result.key = request.key;
          result.presence = query.presence;
          result.evidence = query.detail;
        }
        const std::vector<std::uint8_t> payload = encode(result, config.limits);
        send_payload(session, MessageId::BackendState, payload);
        return true;
      }
      default: {
        bump(counters.protocol_errors);
        send_error(session, StatusCode::ProtocolViolation,
                   std::string("unexpected message ") + to_string(frame.id));
        return false;
      }
    }
  }

  // Bounded, event-driven frame reader. A frame is decoded only when it has been
  // received completely, a partial frame is buffered only up to the configured
  // frame bound, and a shutdown request ends the session explicitly instead of
  // leaving the thread blocked in a receive.
  void session_loop(const std::shared_ptr<Session>& session) {
    std::string why;
    std::vector<std::uint8_t> buffer;
    const std::size_t buffer_bound = config.limits.max_frame_bytes + kFrameHeaderBytes;
    for (;;) {
      if (stopping.load()) {
        break;
      }
      if (buffer.size() >= kFrameHeaderBytes) {
        std::size_t total = 0;
        const FrameStatus header_status =
            frame_header_status(std::span<const std::uint8_t>(buffer.data(), buffer.size()), config.limits, total,
                                why);
        if (header_status == FrameStatus::OversizedPayload) {
          bump(counters.oversized_frames);
          send_error(session, StatusCode::LimitExceeded, why);
          break;
        }
        if (header_status != FrameStatus::Complete) {
          bump(counters.malformed_frames);
          send_error(session, StatusCode::ProtocolViolation, why);
          break;
        }
        if (buffer.size() >= total) {
          DecodedFrame decoded;
          const FrameStatus status =
              decode_frame(std::span<const std::uint8_t>(buffer.data(), total), config.limits, decoded, why);
          if (status != FrameStatus::Complete) {
            if (status == FrameStatus::IntegrityFailure) {
              bump(counters.integrity_failures);
            } else {
              bump(counters.malformed_frames);
            }
            send_error(session, StatusCode::IntegrityFailure, why);
            break;
          }
          buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(total));
          bump(counters.frames_received);
          if (!handle(session, decoded)) {
            break;
          }
          continue;
        }
      }
      const detail::SocketWaiter::WaitStatus wait_status = session->waiter->wait(why);
      if (wait_status != detail::SocketWaiter::WaitStatus::Readable) {
        break;
      }
      std::uint8_t chunk[4096] = {};
      std::size_t received = 0;
      const detail::ReceiveStatus receive =
          detail::receive_some(session->socket, chunk, sizeof(chunk), received, why);
      if (receive == detail::ReceiveStatus::Closed || receive == detail::ReceiveStatus::Failed) {
        break;
      }
      if (receive == detail::ReceiveStatus::WouldBlock) {
        continue;
      }
      if (buffer.size() + received > buffer_bound) {
        bump(counters.oversized_frames);
        send_error(session, StatusCode::LimitExceeded, "the peer sent more bytes than the configured frame bound");
        break;
      }
      buffer.insert(buffer.end(), chunk, chunk + received);
    }
    finish_session(session);
  }

  void finish_session(const std::shared_ptr<Session>& session) {
    if (session->registered) {
      const Expected<PublisherRegistration> registration = runtime->LookupRegistration(session->publisher);
      if (registration && registration.value().worker_boot == session->worker_boot) {
        (void)runtime->FencePublisher(session->publisher, FencingReason::SessionClosed);
      }
    }
    // The handle is cleared under the session lock before it is closed, so a
    // concurrent Stop() can never shut down or close a handle that is already
    // gone (or a handle the operating system has since reused).
    detail::SocketHandle handle = detail::kInvalidSocket;
    {
      std::lock_guard<std::mutex> lock(mutex);
      handle = session->socket;
      session->socket = detail::kInvalidSocket;
      ++counters.sessions_closed;
      if (active_sessions > 0) {
        --active_sessions;
      }
    }
    if (session->waiter != nullptr) {
      session->waiter->close();
    }
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
    session_finished.notify_all();
  }

  void accept_loop() {
    std::string why;
    while (!stopping.load()) {
      sockaddr_storage peer{};
      int peer_size = static_cast<int>(sizeof(peer));
      const detail::SocketHandle client =
          ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (client == detail::kInvalidSocket) {
        break;
      }
      if (stopping.load()) {
        // The wake-up connection used by Stop() to release a pending accept.
        detail::close_socket(client);
        break;
      }
      reap_sessions();
      auto session = std::make_shared<Session>();
      session->socket = client;
      session->waiter = std::make_shared<detail::SocketWaiter>();
      if (!session->waiter->create(client, why)) {
        detail::close_socket(client);
        continue;
      }
      bool accepted = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping.load() && active_sessions < config.limits.max_sessions) {
          ++active_sessions;
          ++counters.sessions_accepted;
          sessions.push_back(session);
          accepted = true;
        } else {
          ++counters.sessions_rejected;
        }
      }
      if (!accepted) {
        detail::close_socket(client);
        continue;
      }
      std::thread(&Impl::session_loop, this, session).detach();
    }
  }

  void reap_sessions() {
    std::lock_guard<std::mutex> lock(mutex);
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [](const std::shared_ptr<Session>& session) {
                                    return session->socket == detail::kInvalidSocket;
                                  }),
                   sessions.end());
  }
};

RouteFabricServer::RouteFabricServer(ServerConfig config, RouteFabricRuntime* runtime,
                                     IRouteProgrammingBackend* backend)
    : impl_(std::make_unique<Impl>(std::move(config), runtime, backend)) {}

RouteFabricServer::~RouteFabricServer() { Stop(); }

Status RouteFabricServer::Start() {
  Impl& impl = *impl_;
  if (impl.runtime == nullptr) {
    return make_error(StatusCode::InvalidArgument, "a runtime is required");
  }
  if (impl.backend == nullptr) {
    return make_error(StatusCode::InvalidArgument, "a programming backend is required");
  }
  if (impl.running.load()) {
    return make_error(StatusCode::AlreadyExists, "the server is already running");
  }
  std::string why;
  bool ipv6 = false;
  if (!detail::create_tcp_socket(impl.config.bind_address, impl.listener, ipv6, why)) {
    return make_error(StatusCode::Internal, why);
  }
  std::uint16_t bound_port = 0;
  if (!detail::bind_and_listen(impl.listener, impl.config.bind_address, impl.config.port, ipv6, bound_port, why)) {
    detail::close_socket(impl.listener);
    impl.listener = detail::kInvalidSocket;
    return make_error(StatusCode::Internal, why);
  }
  impl.port = bound_port;
  impl.stopping.store(false);
  impl.running.store(true);
  impl.accept_thread = std::thread([&impl]() { impl.accept_loop(); });
  return ok_status();
}

void RouteFabricServer::Stop() {
  Impl& impl = *impl_;
  if (!impl.running.load()) {
    return;
  }
  impl.stopping.store(true);
  // A pending accept() is released deterministically by connecting to the
  // listener rather than by closing the socket underneath another thread, which
  // Windows does not guarantee to unblock.
  if (impl.listener != detail::kInvalidSocket && impl.port != 0) {
    std::string wake_address = impl.config.bind_address;
    if (wake_address == "0.0.0.0") {
      wake_address = "127.0.0.1";
    } else if (wake_address == "::" || wake_address == "::0") {
      wake_address = "::1";
    }
    std::string why;
    bool ipv6 = false;
    detail::SocketHandle wake = detail::kInvalidSocket;
    if (detail::create_tcp_socket(wake_address, wake, ipv6, why)) {
      if (!detail::connect_socket(wake, wake_address, impl.port, ipv6, why)) {
        // The listener may already be gone; closing it below still stops the loop.
      }
      detail::close_socket(wake);
    }
  }
  if (impl.accept_thread.joinable()) {
    impl.accept_thread.join();
  }
  detail::shutdown_socket(impl.listener);
  detail::close_socket(impl.listener);
  impl.listener = detail::kInvalidSocket;
  std::vector<std::shared_ptr<Impl::Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    sessions = impl.sessions;
  }
  for (const std::shared_ptr<Impl::Session>& session : sessions) {
    if (session->waiter != nullptr) {
      session->waiter->request_shutdown();
    }
    detail::shutdown_socket(session->socket);
  }
  {
    std::unique_lock<std::mutex> lock(impl.mutex);
    impl.session_finished.wait(lock, [&impl]() { return impl.active_sessions == 0; });
    impl.sessions.clear();
  }
  impl.running.store(false);
}

bool RouteFabricServer::running() const { return impl_->running.load(); }

std::uint16_t RouteFabricServer::port() const { return impl_->port; }

std::size_t RouteFabricServer::session_count() const {
  const Impl& impl = *impl_;
  std::lock_guard<std::mutex> lock(impl.mutex);
  return impl.active_sessions;
}

ServerCounters RouteFabricServer::counters() const {
  const Impl& impl = *impl_;
  std::lock_guard<std::mutex> lock(impl.mutex);
  return impl.counters;
}

}  // namespace routefabric
