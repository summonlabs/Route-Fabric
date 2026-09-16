#ifndef ROUTEFABRIC_CLIENT_HPP
#define ROUTEFABRIC_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "routefabric/backend.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/wire.hpp"

namespace routefabric {

struct ClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  Limits limits;
  // Seed used for this client's mutation attempt identities. Zero selects a
  // platform entropy seed; tests set it for reproducibility.
  std::uint64_t id_seed = 0;
};

// Client for one coordinator session.
//
// Every call performs one request/response exchange. A disconnected peer is an
// explicit error; the client never waits indefinitely and never retries a
// mutation on its own.
class RouteFabricClient {
 public:
  explicit RouteFabricClient(ClientConfig config);
  ~RouteFabricClient();

  RouteFabricClient(const RouteFabricClient&) = delete;
  RouteFabricClient& operator=(const RouteFabricClient&) = delete;

  Status Connect();
  void Close();
  bool connected() const;

  // Authority context sent with every request. The epoch is learned from Hello
  // and may be overridden explicitly (used by tests and by epoch-aware tooling).
  RequestContext& context() { return context_; }
  const RequestContext& context() const { return context_; }
  void set_epoch(const CoordinatorEpoch& epoch) { context_.epoch = epoch; }
  MutationAttemptId next_attempt();

  Expected<HelloResult> Hello(std::string_view client_name);
  Expected<RegisterPublisherResult> RegisterPublisher(const PublisherScope& scope);
  Expected<PublishRouteResult> PublishRoute(const RouteKey& key, const RouteBinding& binding,
                                            const PolicyGeneration& policy_generation,
                                            const RouteGeneration& expected_generation, const RouteId& lineage,
                                            std::string reason);
  Expected<RouteMutationResult> WithdrawRoute(const RouteId& route, const RouteGeneration& expected_generation,
                                              std::string reason);
  Expected<RouteMutationResult> RevalidateRoute(const RouteId& route, std::string reason);
  Expected<RouteMutationResult> RetireRoute(const RouteId& route, std::string reason);
  Expected<RouteMutationResult> RevokeRoute(const RouteId& route, std::string reason);
  Expected<RouteSnapshot> QueryRouteByKey(const RouteKey& key);
  Expected<RouteSnapshot> QueryRouteById(const RouteId& route);
  Expected<RouteExplanation> ExplainRouteByKey(const RouteKey& key);
  Expected<RouteExplanation> ExplainRouteById(const RouteId& route);
  Expected<SnapshotResponse> Snapshot();
  Expected<StatisticsResult> Statistics();
  Expected<CoordinatorEpoch> Epoch();
  Expected<ReconcileResult> ReconcileAll();
  Expected<ReconcileResult> ReconcileRoute(const RouteId& route);
  Expected<ApplyCompletionResult> ApplyCompletion(const ProgrammingCompletion& completion);
  Expected<BackendStateMessage> BackendState();
  Expected<BackendStateMessage> BackendStateFor(const RouteKey& key);

  // Raw frame access used by the protocol integrity tests and by conformance
  // tooling that must be able to emit deliberately malformed frames.
  Status SendFrame(MessageId id, std::span<const std::uint8_t> payload);
  Status SendRawBytes(std::span<const std::uint8_t> frame);
  Expected<DecodedFrame> ReceiveFrame();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  RequestContext context_;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_CLIENT_HPP
