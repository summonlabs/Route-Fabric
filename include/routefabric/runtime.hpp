#ifndef ROUTEFABRIC_RUNTIME_HPP
#define ROUTEFABRIC_RUNTIME_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/persistence.hpp"
#include "routefabric/record.hpp"
#include "routefabric/snapshot.hpp"

namespace routefabric {

// Authority scope of a registered publisher.
//
// The default-constructed scope authorizes nothing: an empty dimension permits
// nothing unless the matching wildcard flag is explicitly set. Route Fabric
// never grants universal wildcard authority implicitly.
struct PublisherScope {
  FabricId fabric;
  std::vector<RoutingNamespace> namespaces;
  std::vector<Destination> destinations;
  std::vector<RouteClass> route_classes;
  bool wildcard_namespaces = false;
  bool wildcard_destinations = false;
  bool wildcard_route_classes = false;
  // Administrative override: permits taking over a route key that is currently
  // owned by another publisher, and minting a new lineage for a retired key.
  bool administrative_override = false;

  bool covers(const RouteKey& key) const noexcept;
  std::string render() const;
};

enum class FencingReason : std::uint8_t {
  SessionClosed = 1,
  WorkerDeath = 2,
  Administrative = 3,
  EpochInvalidated = 4,
};

const char* to_string(FencingReason reason) noexcept;

struct PublisherRegistration {
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  PublisherScope scope;
  bool fenced = false;
  FencingReason fence_reason = FencingReason::SessionClosed;
};

struct RuntimeConfig {
  FabricId fabric;
  Limits limits;
  Durability durability = Durability::None;
  // Store base path used when durability is not None.
  std::filesystem::path store_path;
  // Seed for identity generation. Zero selects a platform entropy seed.
  std::uint64_t id_seed = 0;
  // Number of retained lineage history entries per route (bounded).
  std::size_t history_limit = 32;
};

// Route publication request. Authority context is mandatory: a publication is
// always bound to the current epoch, the registered publisher and the exact
// worker boot that produced it.
struct PublishRequest {
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
  RouteKey key;
  RouteBinding binding;
  PolicyGeneration policy_generation;
  // Optional optimistic concurrency check against the route's current
  // generation. Zero means "no expectation".
  RouteGeneration expected_generation;
  // Optional explicitly minted lineage identity used to recreate a retired key.
  RouteId lineage;
  std::string reason;
};

struct PublishResult {
  RouteId route;
  RouteGeneration generation;
  RouteAuthorityGeneration authority_generation;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  AppliedClassification applied = AppliedClassification::Unknown;
  RouteCurrentness currentness = RouteCurrentness::NotInstalled;
  bool idempotent = false;
  bool replaced = false;
};

struct WithdrawRequest {
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
  RouteId route;
  RouteGeneration expected_generation;
  std::string reason;
};

struct WithdrawOutcome {
  RouteId route;
  RouteGeneration generation;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  AppliedClassification applied = AppliedClassification::Unknown;
  // The route was already withdrawn: the operation was idempotent.
  bool already_withdrawn = false;
  // A withdrawal is already outstanding: no new backend work was dispatched.
  bool withdrawal_in_flight = false;
};

struct RevalidateRequest {
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
  RouteId route;
  std::string reason;
};

struct RetireRequest {
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
  RouteId route;
  std::string reason;
};

struct RevokeRequest {
  PublisherId publisher;
  WorkerBootId worker_boot;
  MutationAttemptId attempt;
  RouteId route;
  std::string reason;
};

// A backend completion delivered after the synchronous window.
struct ProgrammingCompletion {
  ProgrammingAttemptId attempt;
  ProgrammingOutcome outcome = ProgrammingOutcome::Ambiguous;
  std::string detail;
};

enum class CompletionDisposition : std::uint8_t {
  Applied = 1,
  Stale = 2,
  UnknownAttempt = 3,
  RouteMissing = 4,
  LifecycleRejected = 5,
};

const char* to_string(CompletionDisposition disposition) noexcept;

struct ReconciliationSummary {
  std::size_t checked = 0;
  std::size_t matched = 0;
  std::size_t missing = 0;
  std::size_t diverged = 0;
  std::size_t extra = 0;
  std::size_t unavailable = 0;
};

struct RouteListFilter {
  PublisherId publisher;
  bool has_publisher = false;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  bool has_lifecycle = false;
  RouteCurrentness currentness = RouteCurrentness::Current;
  bool has_currentness = false;
  RoutingNamespace routing_namespace;
  bool has_namespace = false;
  std::size_t limit = 0;  // zero means no limit
};

// Local, process-visible counters. Route Fabric transmits no telemetry.
struct RouteCounters {
  std::uint64_t publications = 0;
  std::uint64_t replacements = 0;
  std::uint64_t idempotent_publications = 0;
  std::uint64_t withdrawals = 0;
  std::uint64_t revalidations = 0;
  std::uint64_t retirements = 0;
  std::uint64_t revocations = 0;
  std::uint64_t supersessions = 0;
  std::uint64_t programming_dispatches = 0;
  std::uint64_t programming_deferred = 0;
  std::uint64_t programming_applied = 0;
  std::uint64_t programming_ambiguous = 0;
  std::uint64_t programming_lifecycle_rejected = 0;
  std::uint64_t stale_completions_rejected = 0;
  std::uint64_t stale_authority_rejections = 0;
  std::uint64_t scope_violations = 0;
  std::uint64_t path_authority_rejections = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t reconciliations = 0;
};

struct RouteStatistics {
  CoordinatorEpoch epoch;
  Digest128 state_digest;
  std::size_t route_count = 0;
  std::size_t installed_count = 0;
  std::size_t current_count = 0;
  std::size_t revalidation_required_count = 0;
  std::size_t withdrawn_count = 0;
  std::size_t retired_count = 0;
  std::size_t failed_count = 0;
  std::size_t superseded_count = 0;
  std::size_t publisher_count = 0;
  std::size_t path_dependency_count = 0;
  std::size_t outstanding_programming_count = 0;
  std::size_t revocation_count = 0;
  std::size_t routing_namespace_count = 0;
  RouteCounters counters;
};

// The authoritative route-lifecycle and route-state runtime.
//
// Thread safety:
//   * All public methods are safe to call concurrently from multiple threads.
//   * Mutations are serialized; conflicting operations on one route key resolve
//     deterministically in the order the runtime observes them.
//   * Backend programming calls are always made with the runtime lock released.
//   * Path Authority is queried while the lock is held and must therefore be
//     synchronous and non-blocking. A call back into the runtime from a Path
//     Authority or backend implementation is detected and reported as a
//     ReentrancyViolation instead of deadlocking.
//
// Authority semantics:
//   * Every mutation is bound to the coordinator epoch, the publisher, the
//     worker boot and the mutation attempt that produced it.
//   * A route record being durable does not make it current. Currentness is
//     computed from live registration state, epoch, path authority and backend
//     evidence.
//
// Desired versus applied semantics:
//   * The desired route record is persisted before any backend call.
//   * INSTALLED is only reachable through a backend outcome that reports the
//     desired state as applied; a committed desire never implies installation.
class RouteFabricRuntime {
 public:
  // The backend and path authority must outlive the runtime.
  RouteFabricRuntime(RuntimeConfig config, IRouteProgrammingBackend* backend, IPathAuthority* path_authority);
  ~RouteFabricRuntime();

  RouteFabricRuntime(const RouteFabricRuntime&) = delete;
  RouteFabricRuntime& operator=(const RouteFabricRuntime&) = delete;

  // Loads durable state and performs conservative recovery. Must be called
  // before any query or mutation.
  Status Open();

  // --- authority ---------------------------------------------------------
  Expected<PublisherRegistration> RegisterPublisher(const PublisherId& publisher, const WorkerBootId& worker_boot,
                                                    const PublisherScope& scope);
  // Fences a publisher. SessionClosed/WorkerDeath fencing clears the live
  // registration and bars the fenced worker boot only; a fresh boot may
  // re-register. Administrative fencing bars re-registration until the fence is
  // explicitly cleared.
  Status FencePublisher(const PublisherId& publisher, FencingReason reason);
  Status FenceWorkerBoot(const WorkerBootId& worker_boot, FencingReason reason);
  Status ClearAdministrativeFence(const PublisherId& publisher);
  Expected<PublisherRegistration> LookupRegistration(const PublisherId& publisher) const;
  // Advances the coordinator epoch durably. All existing registrations become
  // stale; routes keep their durable intent but lose live authority.
  Expected<CoordinatorEpoch> AdvanceEpoch();
  CoordinatorEpoch epoch() const;
  // Authority domain of this runtime.
  FabricId fabric() const;

  // --- route lifecycle ---------------------------------------------------
  Expected<PublishResult> PublishRoute(const PublishRequest& request);
  // Batch publication with explicitly independent entries: each entry is
  // validated, committed and programmed on its own, and every entry receives its
  // own result. The batch is bounded by Limits::max_batch_mutations.
  Expected<std::vector<Expected<PublishResult>>> PublishBatch(const std::vector<PublishRequest>& requests);
  Expected<WithdrawOutcome> WithdrawRoute(const WithdrawRequest& request);
  Expected<WithdrawOutcome> RevalidateRoute(const RevalidateRequest& request);
  Expected<WithdrawOutcome> RetireRoute(const RetireRequest& request);
  Expected<WithdrawOutcome> RevokeRoute(const RevokeRequest& request);

  // Delivers a backend completion that arrives after the dispatch window.
  Expected<CompletionDisposition> ApplyProgrammingCompletion(const ProgrammingCompletion& completion);

  // --- path authority ----------------------------------------------------
  // Precise invalidation: only routes bound to the affected path are touched.
  Status OnPathAuthorityChanged(const PathId& path, const PathAuthorityGeneration& generation,
                                PathAuthorization state);
  // Re-queries Path Authority for every route bound to a path and invalidates the
  // ones whose authority is no longer usable. Used after restart, when live
  // authority cannot survive.
  Status RevalidatePathAuthorities();

  // --- reconciliation ----------------------------------------------------
  Expected<ObservationClass> ReconcileRoute(const RouteId& route);
  Expected<ReconciliationSummary> ReconcileAll();

  // --- queries -----------------------------------------------------------
  Expected<RouteSnapshot> QueryRoute(const RouteKey& key) const;
  Expected<RouteSnapshot> QueryRouteById(const RouteId& route) const;
  // Honours the filter's limit and never returns more than
  // Limits::max_snapshot_routes entries.
  std::vector<RouteSnapshot> ListRoutes(const RouteListFilter& filter) const;
  Expected<RouteExplanation> ExplainRoute(const RouteKey& key) const;
  Expected<RouteExplanation> ExplainRouteById(const RouteId& route) const;
  Expected<RouteSnapshotSet> Snapshot() const;
  std::vector<RouteDiffEntry> DiffSnapshots(const RouteSnapshotSet& before, const RouteSnapshotSet& after,
                                            const RouteId& route) const;
  RouteStatistics Statistics() const;
  RouteCounters counters() const;

  // Verifies that every index agrees with the authoritative record map. Exposed
  // so that operators and property tests can assert index consistency.
  Status ValidateIndexes() const;

  // Persists the full state as a new snapshot and clears the journal.
  Status Compact();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_RUNTIME_HPP
