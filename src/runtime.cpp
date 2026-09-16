#include "routefabric/runtime.hpp"

#include <algorithm>
#include <utility>

namespace routefabric {
namespace {

// Detects a call back into the runtime from a backend or path-authority
// implementation. Without this guard such a call would deadlock on the
// non-recursive state lock.
thread_local int t_call_depth = 0;
thread_local bool t_reentrancy_detected = false;

// Detects a call back into the runtime from a backend or path-authority
// implementation. The nested call is refused, and the originating operation is
// failed with a ReentrancyViolation instead of deadlocking on the state lock.
class CallGuard {
 public:
  CallGuard() : reentrant_(t_call_depth > 0) {
    if (reentrant_) {
      t_reentrancy_detected = true;
    } else {
      t_call_depth = 1;
      t_reentrancy_detected = false;
    }
  }
  ~CallGuard() {
    if (!reentrant_) {
      t_call_depth = 0;
    }
  }

  CallGuard(const CallGuard&) = delete;
  CallGuard& operator=(const CallGuard&) = delete;

  bool reentrant() const noexcept { return reentrant_; }
  // True when a nested runtime call was observed during this operation.
  static bool violated() noexcept { return t_reentrancy_detected; }

 private:
  bool reentrant_;
};

Status reentrancy_error() {
  return make_error(StatusCode::ReentrancyViolation,
                    "a backend or path authority implementation called back into RouteFabricRuntime");
}

template <typename T>
Expected<T> reentrancy_error_value() {
  return make_error<T>(StatusCode::ReentrancyViolation,
                       "a backend or path authority implementation called back into RouteFabricRuntime");
}

std::string bounded(std::string text) {
  if (text.size() > kMaxDiagnosticChars) {
    text.resize(kMaxDiagnosticChars);
  }
  return text;
}

// A route that is not path-backed carries path authority generation 1, which
// means "no path authority is involved in this binding".
PathAuthorityGeneration no_path_authority() { return PathAuthorityGeneration::from_value(1); }

RouteEvent install_event(ProgrammingOutcome outcome) {
  switch (outcome) {
    case ProgrammingOutcome::Applied:
      return RouteEvent::BackendApplied;
    case ProgrammingOutcome::Idempotent:
      return RouteEvent::BackendIdempotent;
    case ProgrammingOutcome::NotSupported:
      return RouteEvent::BackendNotSupported;
    case ProgrammingOutcome::Rejected:
      return RouteEvent::BackendRejected;
    case ProgrammingOutcome::RetryableFailure:
      return RouteEvent::BackendRetryableFailure;
    case ProgrammingOutcome::PermanentFailure:
      return RouteEvent::BackendPermanentFailure;
    case ProgrammingOutcome::Ambiguous:
      return RouteEvent::BackendAmbiguous;
    case ProgrammingOutcome::BackendUnavailable:
      return RouteEvent::BackendUnavailable;
  }
  return RouteEvent::BackendAmbiguous;
}

RouteEvent withdraw_event(ProgrammingOutcome outcome) {
  switch (outcome) {
    case ProgrammingOutcome::Applied:
    case ProgrammingOutcome::Idempotent:
      return RouteEvent::WithdrawApplied;
    case ProgrammingOutcome::NotSupported:
    case ProgrammingOutcome::Rejected:
    case ProgrammingOutcome::RetryableFailure:
    case ProgrammingOutcome::PermanentFailure:
    case ProgrammingOutcome::Ambiguous:
    case ProgrammingOutcome::BackendUnavailable:
      return RouteEvent::WithdrawFailed;
  }
  return RouteEvent::WithdrawFailed;
}

AppliedClassification install_classification(ProgrammingOutcome outcome) {
  switch (outcome) {
    case ProgrammingOutcome::Applied:
      return AppliedClassification::Applied;
    case ProgrammingOutcome::Idempotent:
      return AppliedClassification::IdempotentAlreadyApplied;
    case ProgrammingOutcome::NotSupported:
      return AppliedClassification::NotSupported;
    case ProgrammingOutcome::Rejected:
      return AppliedClassification::Rejected;
    case ProgrammingOutcome::RetryableFailure:
      return AppliedClassification::RetryableFailure;
    case ProgrammingOutcome::PermanentFailure:
      return AppliedClassification::PermanentFailure;
    case ProgrammingOutcome::Ambiguous:
      return AppliedClassification::Ambiguous;
    case ProgrammingOutcome::BackendUnavailable:
      return AppliedClassification::BackendUnavailable;
  }
  return AppliedClassification::Ambiguous;
}

AppliedClassification withdraw_classification(ProgrammingOutcome outcome) {
  switch (outcome) {
    case ProgrammingOutcome::Applied:
    case ProgrammingOutcome::Idempotent:
      return AppliedClassification::Withdrawn;
    case ProgrammingOutcome::Ambiguous:
      return AppliedClassification::Ambiguous;
    case ProgrammingOutcome::NotSupported:
    case ProgrammingOutcome::Rejected:
    case ProgrammingOutcome::RetryableFailure:
    case ProgrammingOutcome::PermanentFailure:
    case ProgrammingOutcome::BackendUnavailable:
      return AppliedClassification::WithdrawFailed;
  }
  return AppliedClassification::WithdrawFailed;
}

}  // namespace

const char* to_string(FencingReason reason) noexcept {
  switch (reason) {
    case FencingReason::SessionClosed:
      return "session-closed";
    case FencingReason::WorkerDeath:
      return "worker-death";
    case FencingReason::Administrative:
      return "administrative";
    case FencingReason::EpochInvalidated:
      return "epoch-invalidated";
  }
  return "unknown";
}

const char* to_string(CompletionDisposition disposition) noexcept {
  switch (disposition) {
    case CompletionDisposition::Applied:
      return "applied";
    case CompletionDisposition::Stale:
      return "stale";
    case CompletionDisposition::UnknownAttempt:
      return "unknown-attempt";
    case CompletionDisposition::RouteMissing:
      return "route-missing";
    case CompletionDisposition::LifecycleRejected:
      return "lifecycle-rejected";
  }
  return "unknown";
}

bool PublisherScope::covers(const RouteKey& key) const noexcept {
  if (!key.is_valid() || !fabric.is_valid() || !(fabric == key.fabric)) {
    return false;
  }
  if (!wildcard_namespaces &&
      std::find(namespaces.begin(), namespaces.end(), key.routing_namespace) == namespaces.end()) {
    return false;
  }
  if (!wildcard_route_classes &&
      std::find(route_classes.begin(), route_classes.end(), key.route_class) == route_classes.end()) {
    return false;
  }
  if (!wildcard_destinations) {
    bool covered = false;
    for (const Destination& scope_destination : destinations) {
      if (scope_destination.scope_covers(key.destination)) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      return false;
    }
  }
  return true;
}

std::string PublisherScope::render() const {
  std::string out = "fabric=";
  out += fabric.render();
  out += " namespaces=";
  out += wildcard_namespaces ? std::string("*") : to_decimal(namespaces.size());
  out += " destinations=";
  out += wildcard_destinations ? std::string("*") : to_decimal(destinations.size());
  out += " classes=";
  out += wildcard_route_classes ? std::string("*") : to_decimal(route_classes.size());
  out += " administrative-override=";
  out += administrative_override ? "yes" : "no";
  return out;
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct RouteFabricRuntime::Impl {
  Impl(RuntimeConfig runtime_config, IRouteProgrammingBackend* programming_backend, IPathAuthority* authority)
      : config(std::move(runtime_config)),
        backend(programming_backend),
        path_authority(authority),
        ids(config.id_seed != 0 ? config.id_seed : SeededIdSource::system_seed()) {
    epoch = CoordinatorEpoch::from_value(1);
    policy_generation = PolicyGeneration::from_value(1);
  }

  RuntimeConfig config;
  IRouteProgrammingBackend* backend = nullptr;
  IPathAuthority* path_authority = nullptr;

  mutable std::shared_mutex mutex;
  CoordinatorEpoch epoch;
  PolicyGeneration policy_generation;
  bool opened = false;

  std::map<RouteId, RouteRecord> routes;
  std::map<RouteKey, RouteId> key_index;
  std::map<PathId, std::set<RouteId>> path_index;
  std::map<PublisherId, std::set<RouteId>> publisher_index;
  std::map<RouteLifecycle, std::set<RouteId>> lifecycle_index;
  std::map<RoutingNamespace, std::size_t> namespace_counts;
  std::map<ProgrammingAttemptId, ProgrammingAttemptRecord> attempts;
  std::map<RouteId, RevocationRecord> revocations;
  std::map<PublisherId, PublisherRegistration> registrations;
  std::map<PublisherId, FencingReason> administrative_fences;
  std::set<WorkerBootId> fenced_boots;

  std::unique_ptr<RouteStore> store;
  SeededIdSource ids;
  RouteCounters counters;
  std::size_t outstanding_programming = 0;

  struct LifecycleTarget {
    PublisherRegistration registration;
    RouteRecord record;
  };

  // --- internal helpers ---------------------------------------------------
  Expected<PublishResult> publish_one(const PublishRequest& request);
  Expected<LifecycleTarget> prepare_lifecycle_mutation(const RouteId& route, const PublisherId& publisher,
                                                       const WorkerBootId& worker_boot);
  CompletionDisposition run_dispatch(const RouteId& route_id, const ProgrammingAttemptRecord& attempt,
                                     bool previously_installed, bool& deferred);
  Status invalidate_path_locked(const PathId& path, const PathAuthorityGeneration& generation,
                                PathAuthorization state);
  Expected<ObservationClass> reconcile_one(const RouteId& route);
  RouteExplanation explain_locked(const RouteRecord& record) const;

  void index_erase(const RouteRecord& record) {
    const auto key_it = key_index.find(record.key);
    if (key_it != key_index.end() && key_it->second == record.id) {
      key_index.erase(key_it);
    }
    if (record.binding.kind == BindingKind::AuthorizedPath && record.binding.path.is_valid()) {
      const auto path_it = path_index.find(record.binding.path);
      if (path_it != path_index.end()) {
        path_it->second.erase(record.id);
        if (path_it->second.empty()) {
          path_index.erase(path_it);
        }
      }
    }
    const auto publisher_it = publisher_index.find(record.provenance.publisher);
    if (publisher_it != publisher_index.end()) {
      publisher_it->second.erase(record.id);
      if (publisher_it->second.empty()) {
        publisher_index.erase(publisher_it);
      }
    }
    const auto lifecycle_it = lifecycle_index.find(record.lifecycle);
    if (lifecycle_it != lifecycle_index.end()) {
      lifecycle_it->second.erase(record.id);
      if (lifecycle_it->second.empty()) {
        lifecycle_index.erase(lifecycle_it);
      }
    }
    const auto namespace_it = namespace_counts.find(record.key.routing_namespace);
    if (namespace_it != namespace_counts.end()) {
      if (namespace_it->second > 0) {
        --namespace_it->second;
      }
      if (namespace_it->second == 0) {
        namespace_counts.erase(namespace_it);
      }
    }
  }

  void index_insert(const RouteRecord& record) {
    // A superseded lineage has lost the key to a successor lineage, so it is
    // addressable by identity only.
    if (record.lifecycle != RouteLifecycle::Superseded) {
      key_index[record.key] = record.id;
    }
    if (record.binding.kind == BindingKind::AuthorizedPath && record.binding.path.is_valid()) {
      path_index[record.binding.path].insert(record.id);
    }
    publisher_index[record.provenance.publisher].insert(record.id);
    lifecycle_index[record.lifecycle].insert(record.id);
    ++namespace_counts[record.key.routing_namespace];
  }

  void commit(RouteRecord record) {
    const auto existing = routes.find(record.id);
    if (existing != routes.end()) {
      index_erase(existing->second);
    }
    index_insert(record);
    routes[record.id] = std::move(record);
  }

  Status persist(const std::vector<RouteRecord>& upserts, const std::vector<RevocationRecord>& revoked,
                 const std::vector<ProgrammingAttemptRecord>& attempt_updates) {
    if (store == nullptr) {
      return ok_status();
    }
    if (config.durability == Durability::Snapshot) {
      PersistedState state;
      state.epoch = epoch;
      state.policy_generation = policy_generation;
      for (const auto& pair : routes) {
        state.routes.push_back(pair.second);
      }
      for (const RouteRecord& upsert : upserts) {
        const auto existing =
            std::find_if(state.routes.begin(), state.routes.end(),
                         [&upsert](const RouteRecord& candidate) { return candidate.id == upsert.id; });
        if (existing == state.routes.end()) {
          state.routes.push_back(upsert);
        } else {
          *existing = upsert;
        }
      }
      for (const auto& pair : revocations) {
        state.revocations.push_back(pair.second);
      }
      for (const RevocationRecord& revocation : revoked) {
        const auto existing = std::find_if(state.revocations.begin(), state.revocations.end(),
                                           [&revocation](const RevocationRecord& candidate) {
                                             return candidate.route == revocation.route;
                                           });
        if (existing == state.revocations.end()) {
          state.revocations.push_back(revocation);
        } else {
          *existing = revocation;
        }
      }
      for (const auto& pair : attempts) {
        state.attempts.push_back(pair.second);
      }
      for (const ProgrammingAttemptRecord& attempt : attempt_updates) {
        const auto existing = std::find_if(state.attempts.begin(), state.attempts.end(),
                                           [&attempt](const ProgrammingAttemptRecord& candidate) {
                                             return candidate.attempt == attempt.attempt;
                                           });
        if (existing == state.attempts.end()) {
          state.attempts.push_back(attempt);
        } else {
          *existing = attempt;
        }
      }
      return store->SaveSnapshot(state);
    }
    JournalEntry entry;
    entry.epoch = epoch;
    entry.policy_generation = policy_generation;
    entry.upserts = upserts;
    entry.revocations = revoked;
    entry.attempts = attempt_updates;
    return store->AppendJournal(entry);
  }

  Status compact_store() {
    if (store == nullptr) {
      return ok_status();
    }
    PersistedState state;
    state.epoch = epoch;
    state.policy_generation = policy_generation;
    for (const auto& pair : routes) {
      state.routes.push_back(pair.second);
    }
    for (const auto& pair : revocations) {
      state.revocations.push_back(pair.second);
    }
    for (const auto& pair : attempts) {
      state.attempts.push_back(pair.second);
    }
    return store->SaveSnapshot(state);
  }

  Expected<PublisherRegistration> authorize(const PublisherId& publisher, const WorkerBootId& worker_boot) const {
    const auto fence = administrative_fences.find(publisher);
    if (fence != administrative_fences.end()) {
      return make_error<PublisherRegistration>(
          StatusCode::Fenced, "publisher is administratively fenced: " + std::string(to_string(fence->second)));
    }
    const auto it = registrations.find(publisher);
    if (it == registrations.end()) {
      return make_error<PublisherRegistration>(StatusCode::NotRegistered, "publisher is not registered");
    }
    if (!(it->second.worker_boot == worker_boot)) {
      return make_error<PublisherRegistration>(StatusCode::StaleWorkerBoot,
                                               "the supplied worker boot is not the registered boot for this publisher");
    }
    if (!(it->second.epoch == epoch)) {
      return make_error<PublisherRegistration>(StatusCode::StaleEpoch,
                                               "the publisher registration is bound to a stale coordinator epoch");
    }
    if (fenced_boots.find(worker_boot) != fenced_boots.end()) {
      return make_error<PublisherRegistration>(StatusCode::Fenced, "worker boot has been fenced");
    }
    return it->second;
  }

  RouteCurrentness currentness_of(const RouteRecord& record) const {
    if (revocations.find(record.id) != revocations.end()) {
      return RouteCurrentness::Revoked;
    }
    switch (record.lifecycle) {
      case RouteLifecycle::Retired:
        return RouteCurrentness::Retired;
      case RouteLifecycle::Superseded:
        return RouteCurrentness::Superseded;
      case RouteLifecycle::Failed:
        return RouteCurrentness::Failed;
      case RouteLifecycle::Withdrawn:
        return RouteCurrentness::Withdrawn;
      default:
        break;
    }
    if (!(record.provenance.epoch == epoch)) {
      return RouteCurrentness::StaleEpoch;
    }
    const auto registration = registrations.find(record.provenance.publisher);
    if (registration == registrations.end()) {
      return RouteCurrentness::StalePublisher;
    }
    if (!(registration->second.worker_boot == record.provenance.worker_boot)) {
      return RouteCurrentness::StaleWorkerBoot;
    }
    if (fenced_boots.find(record.provenance.worker_boot) != fenced_boots.end()) {
      return RouteCurrentness::FencedPublisher;
    }
    if (record.binding.kind == BindingKind::AuthorizedPath) {
      const PathAuthorityResult authority = path_authority->Query(record.binding.path);
      if (!authority.usable()) {
        return RouteCurrentness::PathAuthorityRejected;
      }
      if (!(authority.generation == record.binding.path_authority_generation)) {
        return RouteCurrentness::StalePathAuthority;
      }
    }
    if (record.lifecycle == RouteLifecycle::RevalidationRequired) {
      return RouteCurrentness::RevalidationRequired;
    }
    if (record.observation.classification == ObservationClass::Diverged) {
      return RouteCurrentness::StaleBackendObservation;
    }
    if (record.observation.classification == ObservationClass::Missing) {
      return RouteCurrentness::DesiredAppliedMismatch;
    }
    if (record.lifecycle == RouteLifecycle::Installed) {
      return RouteCurrentness::Current;
    }
    return RouteCurrentness::NotInstalled;
  }

  void push_history(RouteRecord& record, RouteEvent event, const RouteGeneration& prior) const {
    LineageEntry entry;
    entry.generation = record.generation;
    entry.prior_generation = prior;
    entry.event = event;
    entry.lifecycle = record.lifecycle;
    entry.binding_kind = record.binding.kind;
    if (record.binding.kind == BindingKind::AuthorizedPath) {
      entry.path = record.binding.path;
      entry.path_authority_generation = record.binding.path_authority_generation;
    }
    entry.publisher = record.provenance.publisher;
    entry.epoch = record.provenance.epoch;
    record.history.push_back(entry);
    if (record.history.size() > config.history_limit) {
      record.history.erase(record.history.begin(), record.history.begin() + static_cast<std::ptrdiff_t>(
                                                                               record.history.size() - config.history_limit));
    }
  }

  Expected<RouteGeneration> advance_generation(const RouteRecord& record) const {
    const Expected<RouteGeneration> next = record.generation.next();
    if (!next) {
      return make_error<RouteGeneration>(StatusCode::StaleGeneration, "route generation would overflow");
    }
    return next.value();
  }

  Expected<RouteAuthorityGeneration> advance_authority(const RouteRecord& record) const {
    const Expected<RouteAuthorityGeneration> next = record.authority_generation.next();
    if (!next) {
      return make_error<RouteAuthorityGeneration>(StatusCode::StaleGeneration,
                                                  "route authority generation would overflow");
    }
    return next.value();
  }

  Expected<ProgrammingGeneration> advance_programming(const RouteRecord& record) const {
    const Expected<ProgrammingGeneration> next = record.programming_generation.next();
    if (!next) {
      return make_error<ProgrammingGeneration>(StatusCode::StaleGeneration, "programming generation would overflow");
    }
    return next.value();
  }

  void record_attempt(const ProgrammingAttemptRecord& attempt) {
    attempts[attempt.attempt] = attempt;
    if (!attempt.resolved) {
      ++outstanding_programming;
    }
  }

  void resolve_attempt(const ProgrammingAttemptRecord& attempt) {
    const auto it = attempts.find(attempt.attempt);
    if (it != attempts.end() && !it->second.resolved) {
      if (outstanding_programming > 0) {
        --outstanding_programming;
      }
    }
    attempts[attempt.attempt] = attempt;
  }

  CompletionDisposition apply_completion_locked(const ProgrammingResult& result) {
    const auto attempt_it = attempts.find(result.attempt);
    if (attempt_it == attempts.end()) {
      return CompletionDisposition::UnknownAttempt;
    }
    ProgrammingAttemptRecord attempt = attempt_it->second;
    if (attempt.resolved) {
      ++counters.stale_completions_rejected;
      return CompletionDisposition::Stale;
    }
    const auto route_it = routes.find(attempt.route);
    if (route_it == routes.end()) {
      return CompletionDisposition::RouteMissing;
    }
    const RouteRecord& current = route_it->second;
    const bool stale = attempt.programming_generation != current.programming_generation ||
                       attempt.desired_generation != current.generation || !(attempt.epoch == epoch) ||
                       current.lifecycle == RouteLifecycle::Retired ||
                       current.lifecycle == RouteLifecycle::Superseded ||
                       revocations.find(current.id) != revocations.end();
    if (stale) {
      attempt.resolved = true;
      const Status status = persist({}, {}, {attempt});
      if (!status) {
        return CompletionDisposition::LifecycleRejected;
      }
      resolve_attempt(attempt);
      ++counters.stale_completions_rejected;
      return CompletionDisposition::Stale;
    }

    const bool withdrawing = attempt.operation == ProgrammingOperation::Withdraw;
    const RouteEvent event = withdrawing ? withdraw_event(result.outcome) : install_event(result.outcome);
    const std::optional<RouteLifecycle> next = next_lifecycle(current.lifecycle, event);
    if (!next.has_value()) {
      attempt.resolved = true;
      const Status status = persist({}, {}, {attempt});
      if (!status) {
        return CompletionDisposition::LifecycleRejected;
      }
      resolve_attempt(attempt);
      ++counters.programming_lifecycle_rejected;
      return CompletionDisposition::LifecycleRejected;
    }

    RouteRecord updated = current;
    updated.lifecycle = *next;
    updated.applied.classification =
        withdrawing ? withdraw_classification(result.outcome) : install_classification(result.outcome);
    updated.applied.attempt = result.attempt;
    updated.applied.programming_generation = attempt.programming_generation;
    updated.applied.programmed_generation = attempt.desired_generation;
    updated.applied.programmed_authority_generation = updated.authority_generation;
    updated.applied.backend = result.backend.is_valid() ? result.backend : backend->id();
    updated.applied.detail = bounded(result.detail);
    push_history(updated, event, RouteGeneration());
    attempt.resolved = true;

    const Status status = persist({updated}, {}, {attempt});
    if (!status) {
      return CompletionDisposition::LifecycleRejected;
    }
    const RouteLifecycle resulting_lifecycle = updated.lifecycle;
    commit(std::move(updated));
    resolve_attempt(attempt);
    if (withdrawing) {
      if (resulting_lifecycle == RouteLifecycle::Withdrawn) {
        ++counters.withdrawals;
      }
    } else {
      ++counters.programming_applied;
      if (install_classification(result.outcome) == AppliedClassification::Ambiguous) {
        ++counters.programming_ambiguous;
      }
    }
    return CompletionDisposition::Applied;
  }

  RouteProgrammingRequest make_programming_request(const RouteRecord& record,
                                                   const ProgrammingAttemptRecord& attempt,
                                                   bool previously_installed) const {
    RouteProgrammingRequest request;
    request.operation = attempt.operation;
    request.attempt = attempt.attempt;
    request.programming_generation = attempt.programming_generation;
    request.route = record.id;
    request.key = record.key;
    request.binding = record.binding;
    request.desired_generation = record.generation;
    request.authority_generation = record.authority_generation;
    request.epoch = epoch;
    request.publisher = record.provenance.publisher;
    request.worker_boot = record.provenance.worker_boot;
    request.previously_installed = previously_installed;
    return request;
  }

  PublishResult publish_result(const RouteRecord& record) const {
    PublishResult result;
    result.route = record.id;
    result.generation = record.generation;
    result.authority_generation = record.authority_generation;
    result.lifecycle = record.lifecycle;
    result.applied = record.applied.classification;
    result.currentness = currentness_of(record);
    return result;
  }

  WithdrawOutcome withdraw_outcome(const RouteRecord& record) const {
    WithdrawOutcome outcome;
    outcome.route = record.id;
    outcome.generation = record.generation;
    outcome.lifecycle = record.lifecycle;
    outcome.applied = record.applied.classification;
    return outcome;
  }

  // Dispatches one programming attempt for a record that is already committed in
  // the INSTALLING or WITHDRAWING lifecycle. Called with the lock released.
  ProgrammingDispatch dispatch(const RouteRecord& record, const ProgrammingAttemptRecord& attempt,
                               bool previously_installed) {
    const RouteProgrammingRequest request = make_programming_request(record, attempt, previously_installed);
    switch (attempt.operation) {
      case ProgrammingOperation::Install:
        return backend->InstallRoute(request);
      case ProgrammingOperation::Replace:
        return backend->ReplaceRoute(request);
      case ProgrammingOperation::Withdraw:
        return backend->WithdrawRoute(request);
    }
    return backend->InstallRoute(request);
  }

  Status validate_common(const PublisherId& publisher, const WorkerBootId& worker_boot,
                         const MutationAttemptId& attempt) const {
    if (!publisher.is_valid()) {
      return make_error(StatusCode::InvalidArgument, "PublisherId must be supplied");
    }
    if (!worker_boot.is_valid()) {
      return make_error(StatusCode::InvalidArgument, "WorkerBootId must be supplied");
    }
    if (!attempt.is_valid()) {
      return make_error(StatusCode::InvalidArgument, "MutationAttemptId must be supplied");
    }
    return ok_status();
  }

  Status check_scope(const PublisherRegistration& registration, const RouteKey& key) {
    if (!registration.scope.covers(key)) {
      ++counters.scope_violations;
      return make_error(StatusCode::ScopeViolation, "publisher scope does not cover route key " + key.render());
    }
    return ok_status();
  }

  void collect_state(PersistedState& state) const {
    state.epoch = epoch;
    state.policy_generation = policy_generation;
    for (const auto& pair : routes) {
      state.routes.push_back(pair.second);
    }
    for (const auto& pair : revocations) {
      state.revocations.push_back(pair.second);
    }
    for (const auto& pair : attempts) {
      state.attempts.push_back(pair.second);
    }
  }
};

// ---------------------------------------------------------------------------
// Construction and durability
// ---------------------------------------------------------------------------

RouteFabricRuntime::RouteFabricRuntime(RuntimeConfig config, IRouteProgrammingBackend* backend,
                                       IPathAuthority* path_authority)
    : impl_(std::make_unique<Impl>(std::move(config), backend, path_authority)) {}

RouteFabricRuntime::~RouteFabricRuntime() = default;

Status RouteFabricRuntime::Open() {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (impl.opened) {
    return make_error(StatusCode::AlreadyExists, "the runtime is already open");
  }
  const Status limits_status = impl.config.limits.validate();
  if (!limits_status) {
    return limits_status;
  }
  if (impl.backend == nullptr) {
    return make_error(StatusCode::InvalidArgument, "a programming backend is required");
  }
  if (impl.path_authority == nullptr) {
    return make_error(StatusCode::InvalidArgument, "a path authority is required");
  }
  if (!impl.config.fabric.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "a fabric identity is required");
  }
  if (impl.config.history_limit == 0 || impl.config.history_limit > impl.config.limits.max_history_entries_per_route) {
    return make_error(StatusCode::InvalidArgument,
                      "history_limit must be within 1..Limits::max_history_entries_per_route");
  }
  if (impl.config.durability != Durability::None) {
    if (impl.config.store_path.empty()) {
      return make_error(StatusCode::InvalidArgument, "durability requires a store path");
    }
    impl.store = std::make_unique<RouteStore>(impl.config.store_path, impl.config.limits, impl.config.durability);
    const Expected<PersistedState> loaded = impl.store->Load();
    if (!loaded) {
      return loaded.error();
    }
    const PersistedState& state = loaded.value();
    impl.epoch = state.epoch;
    impl.policy_generation = state.policy_generation;
    for (const RouteRecord& record : state.routes) {
      impl.routes[record.id] = record;
    }
    for (const RevocationRecord& revocation : state.revocations) {
      impl.revocations[revocation.route] = revocation;
    }
    for (const ProgrammingAttemptRecord& attempt : state.attempts) {
      impl.attempts[attempt.attempt] = attempt;
      if (!attempt.resolved) {
        ++impl.outstanding_programming;
      }
    }
    for (const auto& pair : impl.routes) {
      impl.index_insert(pair.second);
    }
  }

  // Conservative recovery. An unresolved programming attempt means the runtime
  // cannot prove what the backend applied: desired intent survives, applied
  // truth does not.
  std::vector<RouteRecord> recovered;
  std::vector<ProgrammingAttemptRecord> recovered_attempts;
  for (auto& pair : impl.attempts) {
    ProgrammingAttemptRecord attempt = pair.second;
    if (attempt.resolved) {
      continue;
    }
    const auto route_it = impl.routes.find(attempt.route);
    bool touched_route = false;
    if (route_it != impl.routes.end() && has_outstanding_programming(route_it->second.lifecycle) &&
        route_it->second.programming_generation == attempt.programming_generation) {
      RouteRecord record = route_it->second;
      const RouteEvent event = attempt.operation == ProgrammingOperation::Withdraw ? RouteEvent::WithdrawFailed
                                                                                  : RouteEvent::BackendAmbiguous;
      const std::optional<RouteLifecycle> next = next_lifecycle(record.lifecycle, event);
      record.lifecycle = next.has_value() ? *next : RouteLifecycle::RevalidationRequired;
      record.applied.classification = AppliedClassification::Ambiguous;
      record.applied.attempt = attempt.attempt;
      record.applied.programming_generation = attempt.programming_generation;
      record.applied.programmed_generation = attempt.desired_generation;
      record.applied.programmed_authority_generation = record.authority_generation;
      record.applied.backend = impl.backend->id();
      record.applied.detail = "unresolved programming attempt recovered after restart";
      impl.push_history(record, event, RouteGeneration());
      recovered.push_back(record);
      impl.commit(record);
      touched_route = true;
    }
    attempt.resolved = true;
    recovered_attempts.push_back(attempt);
    impl.resolve_attempt(attempt);
    (void)touched_route;
  }
  if (!recovered.empty() || !recovered_attempts.empty()) {
    const Status status = impl.persist(recovered, {}, recovered_attempts);
    if (!status) {
      return status;
    }
  }
  impl.opened = true;
  return ok_status();
}

Status RouteFabricRuntime::Compact() {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error(StatusCode::NotOpen, "the runtime is not open");
  }
  if (impl.store == nullptr) {
    return make_error(StatusCode::Unsupported, "the runtime has no store configured");
  }
  return impl.compact_store();
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

Expected<PublisherRegistration> RouteFabricRuntime::RegisterPublisher(const PublisherId& publisher,
                                                                     const WorkerBootId& worker_boot,
                                                                     const PublisherScope& scope) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<PublisherRegistration>();
  }
  Impl& impl = *impl_;
  if (!publisher.is_valid()) {
    return make_error<PublisherRegistration>(StatusCode::InvalidArgument, "PublisherId must be supplied");
  }
  if (!worker_boot.is_valid()) {
    return make_error<PublisherRegistration>(StatusCode::InvalidArgument, "WorkerBootId must be supplied");
  }
  if (!scope.fabric.is_valid() || !(scope.fabric == impl.config.fabric)) {
    return make_error<PublisherRegistration>(StatusCode::ScopeViolation,
                                             "publisher scope must name the runtime fabric");
  }
  const std::size_t scope_entries = scope.namespaces.size() + scope.destinations.size() + scope.route_classes.size();
  if (scope_entries > impl.config.limits.max_publisher_scopes) {
    return make_error<PublisherRegistration>(StatusCode::LimitExceeded,
                                             "publisher scope exceeds the configured bound");
  }
  if (!scope.administrative_override) {
    if (!scope.wildcard_namespaces && scope.namespaces.empty()) {
      return make_error<PublisherRegistration>(StatusCode::ScopeViolation,
                                               "publisher scope authorizes no routing namespace");
    }
    if (!scope.wildcard_destinations && scope.destinations.empty()) {
      return make_error<PublisherRegistration>(StatusCode::ScopeViolation,
                                               "publisher scope authorizes no destination");
    }
    if (!scope.wildcard_route_classes && scope.route_classes.empty()) {
      return make_error<PublisherRegistration>(StatusCode::ScopeViolation,
                                               "publisher scope authorizes no route class");
    }
  }

  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error<PublisherRegistration>(StatusCode::NotOpen, "the runtime is not open");
  }
  const auto fence = impl.administrative_fences.find(publisher);
  if (fence != impl.administrative_fences.end()) {
    return make_error<PublisherRegistration>(
        StatusCode::Fenced, "publisher is administratively fenced: " + std::string(to_string(fence->second)));
  }
  const auto existing = impl.registrations.find(publisher);
  if (existing == impl.registrations.end() && impl.registrations.size() >= impl.config.limits.max_publishers) {
    return make_error<PublisherRegistration>(StatusCode::LimitExceeded,
                                             "the publisher registration limit has been reached");
  }
  if (existing != impl.registrations.end() && !(existing->second.worker_boot == worker_boot)) {
    // Worker reincarnation: the previous boot is fenced and can never act again.
    impl.fenced_boots.insert(existing->second.worker_boot);
  }
  PublisherRegistration registration;
  registration.publisher = publisher;
  registration.worker_boot = worker_boot;
  registration.epoch = impl.epoch;
  registration.scope = scope;
  registration.fenced = false;
  impl.registrations[publisher] = registration;
  return registration;
}

Status RouteFabricRuntime::FencePublisher(const PublisherId& publisher, FencingReason reason) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  const auto it = impl.registrations.find(publisher);
  if (it != impl.registrations.end()) {
    impl.fenced_boots.insert(it->second.worker_boot);
    impl.registrations.erase(it);
  }
  if (reason == FencingReason::Administrative) {
    impl.administrative_fences[publisher] = reason;
  }
  return ok_status();
}

Status RouteFabricRuntime::FenceWorkerBoot(const WorkerBootId& worker_boot, FencingReason reason) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  if (!worker_boot.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "WorkerBootId must be supplied");
  }
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  impl.fenced_boots.insert(worker_boot);
  if (reason == FencingReason::Administrative) {
    for (auto it = impl.registrations.begin(); it != impl.registrations.end();) {
      if (it->second.worker_boot == worker_boot) {
        impl.administrative_fences[it->first] = reason;
        it = impl.registrations.erase(it);
      } else {
        ++it;
      }
    }
  }
  return ok_status();
}

Status RouteFabricRuntime::ClearAdministrativeFence(const PublisherId& publisher) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  impl.administrative_fences.erase(publisher);
  return ok_status();
}

Expected<PublisherRegistration> RouteFabricRuntime::LookupRegistration(const PublisherId& publisher) const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<PublisherRegistration>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto it = impl.registrations.find(publisher);
  if (it == impl.registrations.end()) {
    return make_error<PublisherRegistration>(StatusCode::NotFound, "publisher is not registered");
  }
  return it->second;
}

Expected<CoordinatorEpoch> RouteFabricRuntime::AdvanceEpoch() {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<CoordinatorEpoch>();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error<CoordinatorEpoch>(StatusCode::NotOpen, "the runtime is not open");
  }
  const Expected<CoordinatorEpoch> next = impl.epoch.next();
  if (!next) {
    return make_error<CoordinatorEpoch>(StatusCode::StaleEpoch, "the coordinator epoch would overflow");
  }
  const CoordinatorEpoch previous = impl.epoch;
  impl.epoch = next.value();
  if (impl.store != nullptr) {
    PersistedState state;
    impl.collect_state(state);
    const Status status = impl.store->SaveSnapshot(state);
    if (!status) {
      impl.epoch = previous;
      return status.error();
    }
  }
  // Every live registration is bound to the previous epoch and is therefore
  // stale; publishers must register again under the new epoch.
  return impl.epoch;
}

CoordinatorEpoch RouteFabricRuntime::epoch() const {
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  return impl.epoch;
}

FabricId RouteFabricRuntime::fabric() const { return impl_->config.fabric; }

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

Expected<PublishResult> RouteFabricRuntime::PublishRoute(const PublishRequest& request) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<PublishResult>();
  }
  return impl_->publish_one(request);
}

Expected<std::vector<Expected<PublishResult>>> RouteFabricRuntime::PublishBatch(
    const std::vector<PublishRequest>& requests) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<std::vector<Expected<PublishResult>>>();
  }
  Impl& impl = *impl_;
  if (requests.size() > impl.config.limits.max_batch_mutations) {
    return make_error<std::vector<Expected<PublishResult>>>(StatusCode::LimitExceeded,
                                                            "the batch exceeds the configured bound");
  }
  std::vector<Expected<PublishResult>> results;
  results.reserve(requests.size());
  for (const PublishRequest& request : requests) {
    results.push_back(impl.publish_one(request));
  }
  return results;
}

Expected<PublishResult> RouteFabricRuntime::Impl::publish_one(const PublishRequest& request) {
  const Status common = validate_common(request.publisher, request.worker_boot, request.attempt);
  if (!common) {
    return common.error();
  }
  if (!request.key.is_valid()) {
    return make_error<PublishResult>(StatusCode::InvalidArgument, "the route key is not valid");
  }
  if (!request.binding.is_valid()) {
    return make_error<PublishResult>(StatusCode::InvalidArgument, "the desired binding is not valid");
  }
  if (!request.policy_generation.is_valid()) {
    return make_error<PublishResult>(StatusCode::InvalidArgument, "a policy generation is required");
  }
  if (request.reason.size() > kMaxDiagnosticChars) {
    return make_error<PublishResult>(StatusCode::LimitExceeded, "the reason text exceeds the configured bound");
  }

  ProgrammingAttemptRecord attempt_record;
  RouteRecord committed;
  RouteGeneration prior_generation;
  bool previously_installed = false;
  bool created = false;
  bool replaced = false;

  {
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (!opened) {
      return make_error<PublishResult>(StatusCode::NotOpen, "the runtime is not open");
    }
    const Expected<PublisherRegistration> registration = authorize(request.publisher, request.worker_boot);
    if (!registration) {
      ++counters.stale_authority_rejections;
      return registration.error();
    }
    const Status scope_status = check_scope(registration.value(), request.key);
    if (!scope_status) {
      return scope_status.error();
    }

    RouteRecord record;
    const auto key_it = key_index.find(request.key);
    if (key_it != key_index.end()) {
      const auto route_it = routes.find(key_it->second);
      if (route_it != routes.end()) {
        record = route_it->second;
      }
    }
    created = !record.id.is_valid();

    if (!created && revocations.find(record.id) != revocations.end()) {
      return make_error<PublishResult>(StatusCode::Revoked,
                                       "the route key has been administratively revoked and cannot be recreated");
    }

    if (!created && record.blocks_republication()) {
      if (!registration.value().scope.administrative_override || !request.lineage.is_valid()) {
        return make_error<PublishResult>(
            StatusCode::Retired,
            "the previous lineage for this route key is no longer usable; an explicit new lineage is required");
      }
      if (request.lineage == record.id) {
        return make_error<PublishResult>(StatusCode::Conflict,
                                         "a new lineage identity must differ from the retired lineage");
      }
      RouteRecord superseded = record;
      const Expected<RouteGeneration> next_generation = advance_generation(superseded);
      if (!next_generation) {
        return next_generation.error();
      }
      const Expected<RouteAuthorityGeneration> next_authority = advance_authority(superseded);
      if (!next_authority) {
        return next_authority.error();
      }
      superseded.generation = next_generation.value();
      superseded.authority_generation = next_authority.value();
      superseded.lifecycle = RouteLifecycle::Superseded;
      superseded.supersession.kind = SupersessionKind::LineageTakeover;
      superseded.supersession.prior_generation = record.generation;
      superseded.supersession.successor_lineage = request.lineage;
      superseded.supersession.reason = bounded(request.reason);
      superseded.supersession.epoch = epoch;
      push_history(superseded, RouteEvent::Supersede, record.generation);
      const Status supersede_status = persist({superseded}, {}, {});
      if (!supersede_status) {
        return supersede_status.error();
      }
      commit(std::move(superseded));
      record = RouteRecord{};
      created = true;
      ++counters.supersessions;
    }

    if (!created) {
      if (!(record.provenance.publisher == request.publisher) &&
          !registration.value().scope.administrative_override) {
        ++counters.conflicts;
        return make_error<PublishResult>(
            StatusCode::Conflict,
            "the route key is exclusively owned by another publisher; administrative override is required");
      }
      if (request.expected_generation.is_valid() && !(request.expected_generation == record.generation)) {
        return make_error<PublishResult>(StatusCode::StaleGeneration,
                                         "expected generation " + request.expected_generation.render() +
                                             " does not match the current generation " + record.generation.render());
      }
      const bool same_binding = record.binding == request.binding;
      const bool same_owner =
          record.provenance.publisher == request.publisher && record.provenance.worker_boot == request.worker_boot;
      const bool settled =
          record.lifecycle == RouteLifecycle::Installed || record.lifecycle == RouteLifecycle::Installing;
      if (same_binding && same_owner && settled) {
        // Exact replay: no generation advance, no new backend dispatch.
        ++counters.idempotent_publications;
        PublishResult result = publish_result(record);
        result.idempotent = true;
        return result;
      }
      previously_installed = record.applied.reports_applied();
      replaced = true;
    }

    if (created) {
      if (routes.size() >= config.limits.max_routes) {
        return make_error<PublishResult>(StatusCode::LimitExceeded, "the route capacity limit has been reached");
      }
      if (namespace_counts.find(request.key.routing_namespace) == namespace_counts.end() &&
          namespace_counts.size() >= config.limits.max_routing_namespaces) {
        return make_error<PublishResult>(StatusCode::LimitExceeded,
                                         "the routing namespace limit has been reached");
      }
      record.id = request.lineage.is_valid() ? request.lineage : derive_route_id(request.key);
      record.key = request.key;
      record.generation = RouteGeneration::from_value(1);
      record.authority_generation = RouteAuthorityGeneration::from_value(1);
      record.invalidation_watermark = RouteAuthorityGeneration::from_value(1);
      record.programming_generation = ProgrammingGeneration::from_value(1);
      record.lifecycle = RouteLifecycle::Declared;
      record.retirement = RetirementRecord{};
      record.supersession = SupersessionRecord{};
      record.observation = BackendObservation{};
      record.history.clear();
      record.provenance = Provenance{};
      RouteLifecycle lifecycle = record.lifecycle;
      if (!apply_event(lifecycle, RouteEvent::BeginValidation) ||
          !apply_event(lifecycle, RouteEvent::ValidationAccepted)) {
        return make_error<PublishResult>(StatusCode::Internal, "the declared lifecycle could not be validated");
      }
      record.lifecycle = lifecycle;
      record.binding = request.binding;
      record.provenance.path_authority_generation = no_path_authority();
    } else {
      // The generation that the caller observed is captured before the record is
      // advanced, so lineage entries and supersession always reference the
      // predecessor generation.
      prior_generation = record.generation;
      const Expected<RouteGeneration> next_generation = advance_generation(record);
      if (!next_generation) {
        return next_generation.error();
      }
      const Expected<RouteAuthorityGeneration> next_authority = advance_authority(record);
      if (!next_authority) {
        return next_authority.error();
      }
      record.generation = next_generation.value();
      record.authority_generation = next_authority.value();
    }

    if (request.binding.kind == BindingKind::AuthorizedPath) {
      const PathAuthorityResult authority = path_authority->Query(request.binding.path);
      if (CallGuard::violated()) {
        return make_error<PublishResult>(
            StatusCode::ReentrancyViolation,
            "the path authority implementation called back into RouteFabricRuntime");
      }
      if (!(authority.generation == request.binding.path_authority_generation)) {
        ++counters.path_authority_rejections;
        return make_error<PublishResult>(
            StatusCode::PathAuthorityStale,
            "the bound path authority generation " + request.binding.path_authority_generation.render() +
                " is not the current generation " + authority.generation.render());
      }
      if (!authority.usable()) {
        ++counters.path_authority_rejections;
        return make_error<PublishResult>(StatusCode::PathAuthorityRejected,
                                         std::string("path authority reports ") + to_string(authority.state));
      }
    }

    RouteRecord updated = record;
    updated.binding = request.binding;
    {
      RouteLifecycle lifecycle = updated.lifecycle;
      const Status event_status = apply_event(lifecycle, RouteEvent::BeginInstall);
      if (!event_status) {
        return event_status.error();
      }
      updated.lifecycle = lifecycle;
    }
    updated.provenance.publisher = request.publisher;
    updated.provenance.worker_boot = request.worker_boot;
    updated.provenance.epoch = epoch;
    updated.provenance.source_class = created && request.lineage.is_valid() ? ProvenanceSourceClass::LineageMint
                                                                           : ProvenanceSourceClass::Publisher;
    updated.provenance.source_generation = updated.generation;
    updated.provenance.mutation_attempt = request.attempt;
    updated.provenance.policy_generation = request.policy_generation;
    updated.provenance.path_authority_generation = request.binding.kind == BindingKind::AuthorizedPath
                                                       ? request.binding.path_authority_generation
                                                       : no_path_authority();
    const Expected<ProgrammingGeneration> next_programming = advance_programming(record);
    if (!next_programming) {
      return next_programming.error();
    }
    updated.programming_generation = next_programming.value();
    const ProgrammingAttemptId programming_attempt = generate_id<ProgrammingAttemptId>(ids);
    updated.applied = AppliedState{};
    updated.applied.classification = AppliedClassification::Pending;
    updated.applied.attempt = programming_attempt;
    updated.applied.programming_generation = updated.programming_generation;
    updated.applied.programmed_generation = updated.generation;
    updated.applied.programmed_authority_generation = updated.authority_generation;
    updated.applied.backend = backend->id();
    updated.observation = BackendObservation{};
    updated.observation.backend = backend->id();
    updated.supersession = SupersessionRecord{};
    if (replaced) {
      updated.supersession.kind = SupersessionKind::Replacement;
      updated.supersession.prior_generation = prior_generation;
      updated.supersession.successor_generation = updated.generation;
      updated.supersession.reason = bounded(request.reason);
      updated.supersession.epoch = epoch;
    }
    push_history(updated, RouteEvent::BeginInstall, replaced ? prior_generation : RouteGeneration());

    const Status record_status = validate_route_record(updated, config.limits);
    if (!record_status) {
      return record_status.error();
    }
    if (outstanding_programming >= config.limits.max_outstanding_programming) {
      return make_error<PublishResult>(StatusCode::LimitExceeded,
                                       "the outstanding programming limit has been reached");
    }

    attempt_record.attempt = programming_attempt;
    attempt_record.route = updated.id;
    attempt_record.desired_generation = updated.generation;
    attempt_record.programming_generation = updated.programming_generation;
    attempt_record.operation = replaced ? ProgrammingOperation::Replace : ProgrammingOperation::Install;
    attempt_record.epoch = epoch;
    attempt_record.resolved = false;

    // Persist the authoritative intent before any backend call and before any
    // acknowledgment.
    const Status persist_status = persist({updated}, {}, {attempt_record});
    if (!persist_status) {
      return persist_status.error();
    }
    commit(updated);
    record_attempt(attempt_record);
    ++counters.programming_dispatches;
    if (created) {
      ++counters.publications;
    } else {
      ++counters.replacements;
    }
    committed = std::move(updated);
  }

  bool deferred = false;
  const CompletionDisposition disposition = run_dispatch(committed.id, attempt_record, previously_installed, deferred);
  if (disposition == CompletionDisposition::RouteMissing) {
    return make_error<PublishResult>(StatusCode::Internal, "the committed route record disappeared");
  }
  std::shared_lock<std::shared_mutex> lock(mutex);
  const auto route_it = routes.find(committed.id);
  if (route_it == routes.end()) {
    return make_error<PublishResult>(StatusCode::Internal, "the committed route record disappeared");
  }
  PublishResult result = publish_result(route_it->second);
  result.replaced = replaced;
  return result;
}

CompletionDisposition RouteFabricRuntime::Impl::run_dispatch(const RouteId& route_id,
                                                             const ProgrammingAttemptRecord& attempt,
                                                             bool previously_installed, bool& deferred) {
  RouteRecord snapshot;
  {
    std::shared_lock<std::shared_mutex> lock(mutex);
    const auto route_it = routes.find(route_id);
    if (route_it == routes.end()) {
      return CompletionDisposition::RouteMissing;
    }
    snapshot = route_it->second;
  }
  // Backend programming happens with the runtime lock released.
  const ProgrammingDispatch dispatch_result = dispatch(snapshot, attempt, previously_installed);
  std::unique_lock<std::shared_mutex> lock(mutex);
  if (dispatch_result.deferred) {
    ++counters.programming_deferred;
    deferred = true;
    return CompletionDisposition::Applied;
  }
  return apply_completion_locked(dispatch_result.immediate);
}

// ---------------------------------------------------------------------------
// Withdrawal, revalidation, retirement, revocation
// ---------------------------------------------------------------------------

Expected<RouteFabricRuntime::Impl::LifecycleTarget> RouteFabricRuntime::Impl::prepare_lifecycle_mutation(
    const RouteId& route, const PublisherId& publisher, const WorkerBootId& worker_boot) {
  const auto route_it = routes.find(route);
  if (route_it == routes.end()) {
    return make_error<LifecycleTarget>(StatusCode::NotFound, "no route with that identity");
  }
  LifecycleTarget target;
  target.record = route_it->second;
  const Expected<PublisherRegistration> found = authorize(publisher, worker_boot);
  if (!found) {
    ++counters.stale_authority_rejections;
    return found.error();
  }
  target.registration = found.value();
  const Status scope_status = check_scope(target.registration, target.record.key);
  if (!scope_status) {
    return scope_status.error();
  }
  if (!(target.record.provenance.publisher == publisher) && !target.registration.scope.administrative_override) {
    ++counters.conflicts;
    return make_error<LifecycleTarget>(
        StatusCode::Conflict,
        "the route is exclusively owned by another publisher; administrative override is required");
  }
  if (revocations.find(target.record.id) != revocations.end()) {
    return make_error<LifecycleTarget>(StatusCode::Revoked, "the route has been administratively revoked");
  }
  if (target.record.lifecycle == RouteLifecycle::Retired) {
    return make_error<LifecycleTarget>(StatusCode::Retired, "the route lineage is retired");
  }
  if (target.record.lifecycle == RouteLifecycle::Superseded) {
    return make_error<LifecycleTarget>(StatusCode::Retired, "the route lineage has been superseded");
  }
  return target;
}

Expected<WithdrawOutcome> RouteFabricRuntime::WithdrawRoute(const WithdrawRequest& request) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<WithdrawOutcome>();
  }
  Impl& impl = *impl_;
  const Status common = impl.validate_common(request.publisher, request.worker_boot, request.attempt);
  if (!common) {
    return common.error();
  }
  if (!request.route.is_valid()) {
    return make_error<WithdrawOutcome>(StatusCode::InvalidArgument, "a route identity is required");
  }
  if (request.reason.size() > kMaxDiagnosticChars) {
    return make_error<WithdrawOutcome>(StatusCode::LimitExceeded, "the reason text exceeds the configured bound");
  }

  ProgrammingAttemptRecord attempt_record;
  RouteRecord committed;
  bool previously_installed = false;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.opened) {
      return make_error<WithdrawOutcome>(StatusCode::NotOpen, "the runtime is not open");
    }
    Expected<Impl::LifecycleTarget> target =
        impl.prepare_lifecycle_mutation(request.route, request.publisher, request.worker_boot);
    if (!target) {
      return target.error();
    }
    RouteRecord record = target.value().record;
    if (request.expected_generation.is_valid() && !(request.expected_generation == record.generation)) {
      return make_error<WithdrawOutcome>(StatusCode::StaleGeneration,
                                         "expected generation " + request.expected_generation.render() +
                                             " does not match the current generation " + record.generation.render());
    }
    if (record.lifecycle == RouteLifecycle::Withdrawn) {
      // Repeated withdrawal is idempotent: no generation advance, no dispatch.
      WithdrawOutcome outcome = impl.withdraw_outcome(record);
      outcome.already_withdrawn = true;
      return outcome;
    }
    if (record.lifecycle == RouteLifecycle::Withdrawing) {
      // A withdrawal is already in flight; the outcome is not yet known and no
      // duplicate backend work is dispatched.
      WithdrawOutcome outcome = impl.withdraw_outcome(record);
      outcome.withdrawal_in_flight = true;
      return outcome;
    }

    RouteRecord updated = record;
    const Expected<RouteGeneration> next_generation = impl.advance_generation(updated);
    if (!next_generation) {
      return next_generation.error();
    }
    const Expected<RouteAuthorityGeneration> next_authority = impl.advance_authority(updated);
    if (!next_authority) {
      return next_authority.error();
    }
    updated.generation = next_generation.value();
    updated.authority_generation = next_authority.value();
    {
      RouteLifecycle lifecycle = updated.lifecycle;
      const Status event_status = apply_event(lifecycle, RouteEvent::BeginWithdraw);
      if (!event_status) {
        return event_status.error();
      }
      updated.lifecycle = lifecycle;
    }
    const Expected<ProgrammingGeneration> next_programming = impl.advance_programming(record);
    if (!next_programming) {
      return next_programming.error();
    }
    updated.programming_generation = next_programming.value();
    const ProgrammingAttemptId programming_attempt = generate_id<ProgrammingAttemptId>(impl.ids);
    previously_installed = updated.applied.reports_applied();
    updated.applied.classification = AppliedClassification::Pending;
    updated.applied.attempt = programming_attempt;
    updated.applied.programming_generation = updated.programming_generation;
    updated.applied.programmed_generation = updated.generation;
    updated.applied.programmed_authority_generation = updated.authority_generation;
    updated.applied.backend = impl.backend->id();
    updated.applied.detail = bounded(request.reason);
    // The previous reconciliation describes state that this dispatch supersedes.
    updated.observation = BackendObservation{};
    updated.observation.backend = impl.backend->id();
    impl.push_history(updated, RouteEvent::BeginWithdraw, record.generation);

    const Status record_status = validate_route_record(updated, impl.config.limits);
    if (!record_status) {
      return record_status.error();
    }
    if (impl.outstanding_programming >= impl.config.limits.max_outstanding_programming) {
      return make_error<WithdrawOutcome>(StatusCode::LimitExceeded,
                                         "the outstanding programming limit has been reached");
    }
    attempt_record.attempt = programming_attempt;
    attempt_record.route = updated.id;
    attempt_record.desired_generation = updated.generation;
    attempt_record.programming_generation = updated.programming_generation;
    attempt_record.operation = ProgrammingOperation::Withdraw;
    attempt_record.epoch = impl.epoch;
    attempt_record.resolved = false;
    const Status persist_status = impl.persist({updated}, {}, {attempt_record});
    if (!persist_status) {
      return persist_status.error();
    }
    impl.commit(updated);
    impl.record_attempt(attempt_record);
    ++impl.counters.programming_dispatches;
    committed = std::move(updated);
  }

  bool deferred = false;
  const CompletionDisposition disposition =
      impl.run_dispatch(committed.id, attempt_record, previously_installed, deferred);
  if (disposition == CompletionDisposition::RouteMissing) {
    return make_error<WithdrawOutcome>(StatusCode::Internal, "the committed route record disappeared");
  }
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto route_it = impl.routes.find(committed.id);
  if (route_it == impl.routes.end()) {
    return make_error<WithdrawOutcome>(StatusCode::Internal, "the committed route record disappeared");
  }
  return impl.withdraw_outcome(route_it->second);
}

Expected<WithdrawOutcome> RouteFabricRuntime::RevalidateRoute(const RevalidateRequest& request) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<WithdrawOutcome>();
  }
  Impl& impl = *impl_;
  const Status common = impl.validate_common(request.publisher, request.worker_boot, request.attempt);
  if (!common) {
    return common.error();
  }
  if (!request.route.is_valid()) {
    return make_error<WithdrawOutcome>(StatusCode::InvalidArgument, "a route identity is required");
  }
  if (request.reason.size() > kMaxDiagnosticChars) {
    return make_error<WithdrawOutcome>(StatusCode::LimitExceeded, "the reason text exceeds the configured bound");
  }

  ProgrammingAttemptRecord attempt_record;
  RouteRecord committed;
  bool previously_installed = false;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.opened) {
      return make_error<WithdrawOutcome>(StatusCode::NotOpen, "the runtime is not open");
    }
    Expected<Impl::LifecycleTarget> target =
        impl.prepare_lifecycle_mutation(request.route, request.publisher, request.worker_boot);
    if (!target) {
      return target.error();
    }
    RouteRecord record = target.value().record;
    if (record.lifecycle == RouteLifecycle::Withdrawn) {
      return make_error<WithdrawOutcome>(StatusCode::LifecycleViolation,
                                         "a withdrawn route must be republished, not revalidated");
    }
    if (record.binding.kind == BindingKind::AuthorizedPath) {
      const PathAuthorityResult authority = impl.path_authority->Query(record.binding.path);
      if (CallGuard::violated()) {
        return make_error<WithdrawOutcome>(
            StatusCode::ReentrancyViolation,
            "the path authority implementation called back into RouteFabricRuntime");
      }
      if (!(authority.generation == record.binding.path_authority_generation)) {
        ++impl.counters.path_authority_rejections;
        return make_error<WithdrawOutcome>(StatusCode::PathAuthorityStale,
                                           "the bound path authority generation is no longer current");
      }
      if (!authority.usable()) {
        ++impl.counters.path_authority_rejections;
        return make_error<WithdrawOutcome>(StatusCode::PathAuthorityRejected,
                                           std::string("path authority reports ") + to_string(authority.state));
      }
    }

    RouteRecord updated = record;
    const Expected<RouteGeneration> next_generation = impl.advance_generation(updated);
    if (!next_generation) {
      return next_generation.error();
    }
    const Expected<RouteAuthorityGeneration> next_authority = impl.advance_authority(updated);
    if (!next_authority) {
      return next_authority.error();
    }
    updated.generation = next_generation.value();
    updated.authority_generation = next_authority.value();
    {
      RouteLifecycle lifecycle = updated.lifecycle;
      const Status event_status = apply_event(lifecycle, RouteEvent::BeginInstall);
      if (!event_status) {
        return event_status.error();
      }
      updated.lifecycle = lifecycle;
    }
    updated.provenance.publisher = request.publisher;
    updated.provenance.worker_boot = request.worker_boot;
    updated.provenance.epoch = impl.epoch;
    updated.provenance.source_class = ProvenanceSourceClass::Publisher;
    updated.provenance.source_generation = updated.generation;
    updated.provenance.mutation_attempt = request.attempt;
    updated.provenance.policy_generation = updated.binding.policy_generation;
    updated.provenance.path_authority_generation = updated.binding.kind == BindingKind::AuthorizedPath
                                                       ? updated.binding.path_authority_generation
                                                       : no_path_authority();
    const Expected<ProgrammingGeneration> next_programming = impl.advance_programming(record);
    if (!next_programming) {
      return next_programming.error();
    }
    updated.programming_generation = next_programming.value();
    const ProgrammingAttemptId programming_attempt = generate_id<ProgrammingAttemptId>(impl.ids);
    previously_installed = updated.applied.reports_applied();
    updated.applied.classification = AppliedClassification::Pending;
    updated.applied.attempt = programming_attempt;
    updated.applied.programming_generation = updated.programming_generation;
    updated.applied.programmed_generation = updated.generation;
    updated.applied.programmed_authority_generation = updated.authority_generation;
    updated.applied.backend = impl.backend->id();
    updated.applied.detail = bounded(request.reason);
    // The previous reconciliation describes state that this dispatch supersedes.
    updated.observation = BackendObservation{};
    updated.observation.backend = impl.backend->id();
    impl.push_history(updated, RouteEvent::BeginInstall, record.generation);

    const Status record_status = validate_route_record(updated, impl.config.limits);
    if (!record_status) {
      return record_status.error();
    }
    if (impl.outstanding_programming >= impl.config.limits.max_outstanding_programming) {
      return make_error<WithdrawOutcome>(StatusCode::LimitExceeded,
                                         "the outstanding programming limit has been reached");
    }
    attempt_record.attempt = programming_attempt;
    attempt_record.route = updated.id;
    attempt_record.desired_generation = updated.generation;
    attempt_record.programming_generation = updated.programming_generation;
    attempt_record.operation =
        previously_installed ? ProgrammingOperation::Replace : ProgrammingOperation::Install;
    attempt_record.epoch = impl.epoch;
    attempt_record.resolved = false;
    const Status persist_status = impl.persist({updated}, {}, {attempt_record});
    if (!persist_status) {
      return persist_status.error();
    }
    impl.commit(updated);
    impl.record_attempt(attempt_record);
    ++impl.counters.programming_dispatches;
    ++impl.counters.revalidations;
    committed = std::move(updated);
  }

  bool deferred = false;
  const CompletionDisposition disposition =
      impl.run_dispatch(committed.id, attempt_record, previously_installed, deferred);
  if (disposition == CompletionDisposition::RouteMissing) {
    return make_error<WithdrawOutcome>(StatusCode::Internal, "the committed route record disappeared");
  }
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto route_it = impl.routes.find(committed.id);
  if (route_it == impl.routes.end()) {
    return make_error<WithdrawOutcome>(StatusCode::Internal, "the committed route record disappeared");
  }
  return impl.withdraw_outcome(route_it->second);
}

Expected<WithdrawOutcome> RouteFabricRuntime::RetireRoute(const RetireRequest& request) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<WithdrawOutcome>();
  }
  Impl& impl = *impl_;
  const Status common = impl.validate_common(request.publisher, request.worker_boot, request.attempt);
  if (!common) {
    return common.error();
  }
  if (!request.route.is_valid()) {
    return make_error<WithdrawOutcome>(StatusCode::InvalidArgument, "a route identity is required");
  }
  if (request.reason.size() > kMaxDiagnosticChars) {
    return make_error<WithdrawOutcome>(StatusCode::LimitExceeded, "the reason text exceeds the configured bound");
  }
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error<WithdrawOutcome>(StatusCode::NotOpen, "the runtime is not open");
  }
  Expected<Impl::LifecycleTarget> target =
      impl.prepare_lifecycle_mutation(request.route, request.publisher, request.worker_boot);
  if (!target) {
    return target.error();
  }
  RouteRecord record = target.value().record;
  if (record.lifecycle == RouteLifecycle::Retired) {
    // Repeated retirement is idempotent.
    return impl.withdraw_outcome(record);
  }
  RouteRecord updated = record;
  const Expected<RouteGeneration> next_generation = impl.advance_generation(updated);
  if (!next_generation) {
    return next_generation.error();
  }
  const Expected<RouteAuthorityGeneration> next_authority = impl.advance_authority(updated);
  if (!next_authority) {
    return next_authority.error();
  }
  updated.generation = next_generation.value();
  updated.authority_generation = next_authority.value();
  {
    RouteLifecycle lifecycle = updated.lifecycle;
    const Status event_status = apply_event(lifecycle, RouteEvent::Retire);
    if (!event_status) {
      return event_status.error();
    }
    updated.lifecycle = lifecycle;
  }
  updated.retirement.cause = RetirementCause::Administrative;
  updated.retirement.reason = bounded(request.reason);
  updated.retirement.epoch = impl.epoch;
  updated.retirement.generation = updated.generation;
  updated.retirement.attempt = request.attempt;
  impl.push_history(updated, RouteEvent::Retire, record.generation);

  const Status record_status = validate_route_record(updated, impl.config.limits);
  if (!record_status) {
    return record_status.error();
  }
  const Status persist_status = impl.persist({updated}, {}, {});
  if (!persist_status) {
    return persist_status.error();
  }
  impl.commit(updated);
  ++impl.counters.retirements;
  return impl.withdraw_outcome(updated);
}

Expected<WithdrawOutcome> RouteFabricRuntime::RevokeRoute(const RevokeRequest& request) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<WithdrawOutcome>();
  }
  Impl& impl = *impl_;
  const Status common = impl.validate_common(request.publisher, request.worker_boot, request.attempt);
  if (!common) {
    return common.error();
  }
  if (!request.route.is_valid()) {
    return make_error<WithdrawOutcome>(StatusCode::InvalidArgument, "a route identity is required");
  }
  if (request.reason.size() > kMaxDiagnosticChars) {
    return make_error<WithdrawOutcome>(StatusCode::LimitExceeded, "the reason text exceeds the configured bound");
  }
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error<WithdrawOutcome>(StatusCode::NotOpen, "the runtime is not open");
  }
  const auto route_it = impl.routes.find(request.route);
  if (route_it == impl.routes.end()) {
    return make_error<WithdrawOutcome>(StatusCode::NotFound, "no route with that identity");
  }
  const Expected<PublisherRegistration> registration = impl.authorize(request.publisher, request.worker_boot);
  if (!registration) {
    ++impl.counters.stale_authority_rejections;
    return registration.error();
  }
  if (!registration.value().scope.administrative_override) {
    ++impl.counters.conflicts;
    return make_error<WithdrawOutcome>(StatusCode::Unauthorized,
                                       "revocation requires an administrative override scope");
  }
  RouteRecord record = route_it->second;
  if (impl.revocations.find(record.id) != impl.revocations.end()) {
    // Repeated revocation is idempotent.
    return impl.withdraw_outcome(record);
  }
  RouteRecord updated = record;
  const Expected<RouteGeneration> next_generation = impl.advance_generation(updated);
  if (!next_generation) {
    return next_generation.error();
  }
  const Expected<RouteAuthorityGeneration> next_authority = impl.advance_authority(updated);
  if (!next_authority) {
    return next_authority.error();
  }
  updated.generation = next_generation.value();
  updated.authority_generation = next_authority.value();
  {
    RouteLifecycle lifecycle = updated.lifecycle;
    const Status event_status = apply_event(lifecycle, RouteEvent::Revoke);
    if (!event_status) {
      return event_status.error();
    }
    updated.lifecycle = lifecycle;
  }
  updated.retirement.cause = RetirementCause::Revoked;
  updated.retirement.reason = bounded(request.reason);
  updated.retirement.epoch = impl.epoch;
  updated.retirement.generation = updated.generation;
  updated.retirement.attempt = request.attempt;
  impl.push_history(updated, RouteEvent::Revoke, record.generation);

  RevocationRecord revocation;
  revocation.route = updated.id;
  revocation.key = updated.key;
  revocation.generation = updated.generation;
  revocation.epoch = impl.epoch;
  revocation.attempt = request.attempt;
  revocation.reason = bounded(request.reason);

  const Status record_status = validate_route_record(updated, impl.config.limits);
  if (!record_status) {
    return record_status.error();
  }
  const Status persist_status = impl.persist({updated}, {revocation}, {});
  if (!persist_status) {
    return persist_status.error();
  }
  impl.commit(updated);
  impl.revocations[revocation.route] = revocation;
  ++impl.counters.revocations;
  return impl.withdraw_outcome(updated);
}

Expected<CompletionDisposition> RouteFabricRuntime::ApplyProgrammingCompletion(
    const ProgrammingCompletion& completion) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<CompletionDisposition>();
  }
  Impl& impl = *impl_;
  if (!completion.attempt.is_valid()) {
    return make_error<CompletionDisposition>(StatusCode::InvalidArgument,
                                             "a programming attempt identity is required");
  }
  if (completion.detail.size() > kMaxDiagnosticChars) {
    return make_error<CompletionDisposition>(StatusCode::LimitExceeded,
                                             "the detail text exceeds the configured bound");
  }
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error<CompletionDisposition>(StatusCode::NotOpen, "the runtime is not open");
  }
  ProgrammingResult result;
  result.attempt = completion.attempt;
  result.outcome = completion.outcome;
  result.backend = impl.backend->id();
  result.detail = completion.detail;
  return impl.apply_completion_locked(result);
}

// ---------------------------------------------------------------------------
// Path authority integration
// ---------------------------------------------------------------------------

Status RouteFabricRuntime::Impl::invalidate_path_locked(const PathId& path, const PathAuthorityGeneration& generation,
                                                        PathAuthorization state) {
  const auto path_it = path_index.find(path);
  if (path_it == path_index.end()) {
    return ok_status();
  }
  const std::vector<RouteId> dependents(path_it->second.begin(), path_it->second.end());
  std::vector<RouteRecord> updated_records;
  updated_records.reserve(dependents.size());
  for (const RouteId& id : dependents) {
    const auto route_it = routes.find(id);
    if (route_it == routes.end()) {
      continue;
    }
    RouteRecord record = route_it->second;
    if (revocations.find(id) != revocations.end()) {
      continue;
    }
    // Only routes that still assert installability are invalidated. Retired,
    // superseded, failed, withdrawn and in-flight withdrawals have no
    // installable authority left to invalidate.
    switch (record.lifecycle) {
      case RouteLifecycle::Installed:
      case RouteLifecycle::Installing:
      case RouteLifecycle::Ready:
      case RouteLifecycle::RevalidationRequired:
        break;
      default:
        continue;
    }
    const bool still_usable =
        state == PathAuthorization::Usable && generation == record.binding.path_authority_generation;
    if (still_usable) {
      continue;
    }
    const Expected<RouteGeneration> next_generation = advance_generation(record);
    if (!next_generation) {
      return next_generation.error();
    }
    const Expected<RouteAuthorityGeneration> next_authority = advance_authority(record);
    if (!next_authority) {
      return next_authority.error();
    }
    record.generation = next_generation.value();
    record.authority_generation = next_authority.value();
    record.invalidation_watermark = record.authority_generation;
    record.provenance.path_authority_generation = generation;
    RouteLifecycle lifecycle = record.lifecycle;
    const Status event_status = apply_event(lifecycle, RouteEvent::RequireRevalidation);
    if (!event_status) {
      return event_status.error();
    }
    record.lifecycle = lifecycle;
    push_history(record, RouteEvent::RequireRevalidation, route_it->second.generation);
    updated_records.push_back(std::move(record));
  }
  if (updated_records.empty()) {
    return ok_status();
  }
  const Status persist_status = persist(updated_records, {}, {});
  if (!persist_status) {
    return persist_status;
  }
  for (RouteRecord& record : updated_records) {
    commit(std::move(record));
  }
  return ok_status();
}

Status RouteFabricRuntime::OnPathAuthorityChanged(const PathId& path, const PathAuthorityGeneration& generation,
                                                  PathAuthorization state) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  if (!path.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "a path identity is required");
  }
  if (!generation.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "a path authority generation is required");
  }
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error(StatusCode::NotOpen, "the runtime is not open");
  }
  return impl.invalidate_path_locked(path, generation, state);
}

Status RouteFabricRuntime::RevalidatePathAuthorities() {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.opened) {
    return make_error(StatusCode::NotOpen, "the runtime is not open");
  }
  const std::vector<PathId> paths = [&impl]() {
    std::vector<PathId> collected;
    collected.reserve(impl.path_index.size());
    for (const auto& pair : impl.path_index) {
      collected.push_back(pair.first);
    }
    return collected;
  }();
  for (const PathId& path : paths) {
    const PathAuthorityResult authority = impl.path_authority->Query(path);
    const Status status = impl.invalidate_path_locked(path, authority.generation, authority.state);
    if (!status) {
      return status;
    }
  }
  return ok_status();
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

Expected<ObservationClass> RouteFabricRuntime::Impl::reconcile_one(const RouteId& route) {
  RouteKey key;
  {
    std::shared_lock<std::shared_mutex> lock(mutex);
    const auto route_it = routes.find(route);
    if (route_it == routes.end()) {
      return make_error<ObservationClass>(StatusCode::NotFound, "no route with that identity");
    }
    key = route_it->second.key;
  }
  // The backend query happens with the runtime lock released.
  const BackendQueryResult query = backend->QueryRoute(key);

  std::unique_lock<std::shared_mutex> lock(mutex);
  const auto route_it = routes.find(route);
  if (route_it == routes.end()) {
    return make_error<ObservationClass>(StatusCode::NotFound, "no route with that identity");
  }
  RouteRecord updated = route_it->second;
  ObservationClass classification = ObservationClass::Unknown;
  const bool desired_present = updated.lifecycle == RouteLifecycle::Installed ||
                               updated.lifecycle == RouteLifecycle::Installing ||
                               updated.lifecycle == RouteLifecycle::RevalidationRequired;
  if (query.presence == BackendPresence::Unavailable) {
    classification = ObservationClass::Unavailable;
  } else if (query.presence == BackendPresence::Unknown) {
    classification = ObservationClass::Unknown;
  } else if (query.observed_generation.is_valid() && !(query.observed_generation == updated.generation)) {
    classification = ObservationClass::Diverged;
  } else if (query.presence == BackendPresence::Present) {
    classification = desired_present ? ObservationClass::Matched : ObservationClass::Extra;
  } else {
    classification = desired_present ? ObservationClass::Missing : ObservationClass::Matched;
  }
  updated.observation.classification = classification;
  updated.observation.attempt = updated.applied.attempt;
  updated.observation.observed_generation = (query.observed_generation.is_valid() &&
                                             query.observed_generation <= updated.generation)
                                                ? query.observed_generation
                                                : RouteGeneration();
  updated.observation.backend = query.backend.is_valid() ? query.backend : backend->id();
  updated.observation.detail = bounded(query.detail);
  const Status record_status = validate_route_record(updated, config.limits);
  if (!record_status) {
    return record_status.error();
  }
  const Status persist_status = persist({updated}, {}, {});
  if (!persist_status) {
    return persist_status.error();
  }
  commit(std::move(updated));
  ++counters.reconciliations;
  return classification;
}

Expected<ObservationClass> RouteFabricRuntime::ReconcileRoute(const RouteId& route) {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<ObservationClass>();
  }
  Impl& impl = *impl_;
  if (!route.is_valid()) {
    return make_error<ObservationClass>(StatusCode::InvalidArgument, "a route identity is required");
  }
  return impl.reconcile_one(route);
}

Expected<ReconciliationSummary> RouteFabricRuntime::ReconcileAll() {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<ReconciliationSummary>();
  }
  Impl& impl = *impl_;
  std::vector<RouteId> ids;
  {
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.opened) {
      return make_error<ReconciliationSummary>(StatusCode::NotOpen, "the runtime is not open");
    }
    ids.reserve(impl.routes.size());
    for (const auto& pair : impl.routes) {
      ids.push_back(pair.first);
    }
  }
  ReconciliationSummary summary;
  for (const RouteId& id : ids) {
    const Expected<ObservationClass> classification = impl.reconcile_one(id);
    if (!classification) {
      continue;
    }
    ++summary.checked;
    switch (classification.value()) {
      case ObservationClass::Matched:
        ++summary.matched;
        break;
      case ObservationClass::Missing:
        ++summary.missing;
        break;
      case ObservationClass::Diverged:
        ++summary.diverged;
        break;
      case ObservationClass::Extra:
        ++summary.extra;
        break;
      case ObservationClass::Unknown:
      case ObservationClass::Unavailable:
        ++summary.unavailable;
        break;
    }
  }
  return summary;
}

// ---------------------------------------------------------------------------
// Queries, snapshots and explanations
// ---------------------------------------------------------------------------

RouteExplanation RouteFabricRuntime::Impl::explain_locked(const RouteRecord& record) const {
  RouteExplanation explanation;
  const RouteCurrentness currentness = currentness_of(record);
  explanation.snapshot = make_snapshot(record, currentness);
  explanation.authoritative = is_current(currentness);
  const auto registration = registrations.find(record.provenance.publisher);

  std::string authority_reason;
  authority_reason += "route ";
  authority_reason += record.id.render();
  authority_reason += " is ";
  authority_reason += explanation.authoritative ? "authoritative" : "not authoritative";
  authority_reason += "; lifecycle=";
  authority_reason += to_string(record.lifecycle);
  authority_reason += " currentness=";
  authority_reason += to_string(currentness);
  authority_reason += " owner=";
  authority_reason += record.provenance.publisher.render();
  authority_reason += " worker-boot=";
  authority_reason += record.provenance.worker_boot.render();
  authority_reason += " epoch=";
  authority_reason += record.provenance.epoch.render();
  if (registration == registrations.end()) {
    authority_reason += " registration=none";
  } else {
    authority_reason += " registration-epoch=";
    authority_reason += registration->second.epoch.render();
    authority_reason += " registration-boot=";
    authority_reason += registration->second.worker_boot.render();
  }
  explanation.authority_reason = authority_reason;

  switch (currentness) {
    case RouteCurrentness::Current:
      explanation.currentness_reason = "the backend reported the desired binding as applied and live authority is current";
      break;
    case RouteCurrentness::NotInstalled:
      explanation.currentness_reason = "the route has no applied backend outcome yet";
      break;
    case RouteCurrentness::RevalidationRequired:
      explanation.currentness_reason = "the applied state cannot be proven; an explicit revalidation is required";
      break;
    case RouteCurrentness::StaleEpoch:
      explanation.currentness_reason =
          "the route was published under epoch " + record.provenance.epoch.render() +
          " and the coordinator epoch is now " + epoch.render();
      break;
    case RouteCurrentness::StalePublisher:
      explanation.currentness_reason = "the owning publisher has no live registration after restart or fencing";
      break;
    case RouteCurrentness::StaleWorkerBoot:
      explanation.currentness_reason = "the owning worker boot is not the currently registered boot for the publisher";
      break;
    case RouteCurrentness::StalePathAuthority:
      explanation.currentness_reason = "the bound path authority generation is no longer the current generation";
      break;
    case RouteCurrentness::StaleBackendObservation:
      explanation.currentness_reason = "backend reconciliation observed a divergent state";
      break;
    case RouteCurrentness::FencedPublisher:
      explanation.currentness_reason = "the producing worker boot has been fenced";
      break;
    case RouteCurrentness::PathAuthorityRejected:
      explanation.currentness_reason = "path authority no longer authorizes the bound path";
      break;
    case RouteCurrentness::Withdrawn:
      explanation.currentness_reason = "the route was withdrawn and the backend reported it removed";
      break;
    case RouteCurrentness::Retired:
      explanation.currentness_reason = "the route lineage is retired";
      break;
    case RouteCurrentness::Superseded:
      explanation.currentness_reason = "the route lineage was superseded by a successor lineage";
      break;
    case RouteCurrentness::Failed:
      explanation.currentness_reason = "the backend definitively refused the desired binding";
      break;
    case RouteCurrentness::Revoked:
      explanation.currentness_reason = "the route was administratively revoked";
      break;
    case RouteCurrentness::DesiredAppliedMismatch:
      explanation.currentness_reason = "reconciliation found the desired route missing from the backend";
      break;
  }

  std::string lifecycle_reason = "lifecycle=";
  lifecycle_reason += to_string(record.lifecycle);
  lifecycle_reason += " generation=";
  lifecycle_reason += record.generation.render();
  lifecycle_reason += " authority-generation=";
  lifecycle_reason += record.authority_generation.render();
  lifecycle_reason += " invalidation-watermark=";
  lifecycle_reason += record.invalidation_watermark.render();
  lifecycle_reason += " history=";
  lifecycle_reason += to_decimal(record.history.size());
  if (!record.history.empty()) {
    lifecycle_reason += " last-event=";
    lifecycle_reason += to_string(record.history.back().event);
  }
  explanation.lifecycle_reason = lifecycle_reason;

  if (record.binding.kind == BindingKind::AuthorizedPath) {
    const PathAuthorityResult authority = path_authority->Query(record.binding.path);
    std::string path_reason = "path=";
    path_reason += record.binding.path.render();
    path_reason += " bound-generation=";
    path_reason += record.binding.path_authority_generation.render();
    path_reason += " current-generation=";
    path_reason += authority.generation.render();
    path_reason += " state=";
    path_reason += to_string(authority.state);
    explanation.path_authority_reason = path_reason;
  } else {
    explanation.path_authority_reason = "the binding is not path-backed; no path authority is involved";
  }

  std::string backend_reason = "backend=";
  backend_reason += record.applied.backend.render();
  backend_reason += " applied=";
  backend_reason += to_string(record.applied.classification);
  backend_reason += " attempt=";
  backend_reason += record.applied.attempt.is_valid() ? record.applied.attempt.render() : "none";
  backend_reason += " programmed-generation=";
  backend_reason += record.applied.programmed_generation.is_valid() ? record.applied.programmed_generation.render()
                                                                   : "none";
  backend_reason += " programming-generation=";
  backend_reason += record.programming_generation.render();
  backend_reason += " detail=";
  backend_reason += record.applied.detail.empty() ? "none" : record.applied.detail;
  explanation.backend_reason = backend_reason;

  std::string reconciliation_reason = "observation=";
  reconciliation_reason += to_string(record.observation.classification);
  reconciliation_reason += " backend=";
  reconciliation_reason += record.observation.backend.render();
  reconciliation_reason += " detail=";
  reconciliation_reason += record.observation.detail.empty() ? "none" : record.observation.detail;
  reconciliation_reason += " backend-matched-desired=";
  reconciliation_reason += record.observation.classification == ObservationClass::Matched ? "yes" : "no";
  explanation.reconciliation_reason = reconciliation_reason;

  if (record.supersession.kind == SupersessionKind::None) {
    explanation.supersession_reason = "none";
  } else {
    std::string supersession = "kind=";
    supersession += to_string(record.supersession.kind);
    supersession += " prior-generation=";
    supersession += record.supersession.prior_generation.is_valid() ? record.supersession.prior_generation.render()
                                                                    : "none";
    supersession += " successor-generation=";
    supersession += record.supersession.successor_generation.is_valid()
                        ? record.supersession.successor_generation.render()
                        : "none";
    supersession += " successor-lineage=";
    supersession += record.supersession.successor_lineage.is_valid() ? record.supersession.successor_lineage.render()
                                                                     : "none";
    supersession += " reason=";
    supersession += record.supersession.reason;
    explanation.supersession_reason = supersession;
  }

  if (record.retirement.cause == RetirementCause::None) {
    explanation.retirement_reason = "none";
  } else {
    std::string retirement = "cause=";
    retirement += to_string(record.retirement.cause);
    retirement += " generation=";
    retirement += record.retirement.generation.is_valid() ? record.retirement.generation.render() : "none";
    retirement += " epoch=";
    retirement += record.retirement.epoch.is_valid() ? record.retirement.epoch.render() : "none";
    retirement += " reason=";
    retirement += record.retirement.reason;
    const auto revocation = revocations.find(record.id);
    retirement += " revocation=";
    retirement += revocation == revocations.end() ? "none" : "durable";
    explanation.retirement_reason = retirement;
  }
  return explanation;
}

Expected<RouteSnapshot> RouteFabricRuntime::QueryRoute(const RouteKey& key) const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<RouteSnapshot>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto key_it = impl.key_index.find(key);
  if (key_it == impl.key_index.end()) {
    return make_error<RouteSnapshot>(StatusCode::NotFound, "no route for that key");
  }
  const auto route_it = impl.routes.find(key_it->second);
  if (route_it == impl.routes.end()) {
    return make_error<RouteSnapshot>(StatusCode::NotFound, "no route for that key");
  }
  return make_snapshot(route_it->second, impl.currentness_of(route_it->second));
}

Expected<RouteSnapshot> RouteFabricRuntime::QueryRouteById(const RouteId& route) const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<RouteSnapshot>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto route_it = impl.routes.find(route);
  if (route_it == impl.routes.end()) {
    return make_error<RouteSnapshot>(StatusCode::NotFound, "no route with that identity");
  }
  return make_snapshot(route_it->second, impl.currentness_of(route_it->second));
}

Expected<RouteExplanation> RouteFabricRuntime::ExplainRoute(const RouteKey& key) const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<RouteExplanation>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto key_it = impl.key_index.find(key);
  if (key_it == impl.key_index.end()) {
    return make_error<RouteExplanation>(StatusCode::NotFound, "no route for that key");
  }
  const auto route_it = impl.routes.find(key_it->second);
  if (route_it == impl.routes.end()) {
    return make_error<RouteExplanation>(StatusCode::NotFound, "no route for that key");
  }
  return impl.explain_locked(route_it->second);
}

Expected<RouteExplanation> RouteFabricRuntime::ExplainRouteById(const RouteId& route) const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<RouteExplanation>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const auto route_it = impl.routes.find(route);
  if (route_it == impl.routes.end()) {
    return make_error<RouteExplanation>(StatusCode::NotFound, "no route with that identity");
  }
  return impl.explain_locked(route_it->second);
}

std::vector<RouteSnapshot> RouteFabricRuntime::ListRoutes(const RouteListFilter& filter) const {
  CallGuard guard;
  std::vector<RouteSnapshot> snapshots;
  if (guard.reentrant()) {
    return snapshots;
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const std::size_t hard_cap = impl.config.limits.max_snapshot_routes;
  const std::size_t soft_cap = filter.limit == 0 ? hard_cap : std::min(filter.limit, hard_cap);
  for (const auto& pair : impl.routes) {
    const RouteRecord& record = pair.second;
    if (filter.has_publisher && !(record.provenance.publisher == filter.publisher)) {
      continue;
    }
    if (filter.has_lifecycle && record.lifecycle != filter.lifecycle) {
      continue;
    }
    if (filter.has_namespace && !(record.key.routing_namespace == filter.routing_namespace)) {
      continue;
    }
    const RouteCurrentness currentness = impl.currentness_of(record);
    if (filter.has_currentness && currentness != filter.currentness) {
      continue;
    }
    snapshots.push_back(make_snapshot(record, currentness));
    if (snapshots.size() >= soft_cap) {
      break;
    }
  }
  return snapshots;
}

Expected<RouteSnapshotSet> RouteFabricRuntime::Snapshot() const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error_value<RouteSnapshotSet>();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  if (impl.routes.size() > impl.config.limits.max_snapshot_routes) {
    return make_error<RouteSnapshotSet>(StatusCode::LimitExceeded,
                                        "the snapshot exceeds the configured route bound");
  }
  std::vector<RouteRecord> records;
  std::vector<RouteCurrentness> currentness;
  records.reserve(impl.routes.size());
  currentness.reserve(impl.routes.size());
  for (const auto& pair : impl.routes) {
    records.push_back(pair.second);
  }
  for (const RouteRecord& record : records) {
    currentness.push_back(impl.currentness_of(record));
  }
  return make_snapshot_set(records, currentness, impl.epoch);
}

std::vector<RouteDiffEntry> RouteFabricRuntime::DiffSnapshots(const RouteSnapshotSet& before,
                                                              const RouteSnapshotSet& after,
                                                              const RouteId& route) const {
  return diff_snapshot_sets(before, after, route);
}

RouteStatistics RouteFabricRuntime::Statistics() const {
  CallGuard guard;
  RouteStatistics statistics;
  if (guard.reentrant()) {
    return statistics;
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  statistics.epoch = impl.epoch;
  statistics.route_count = impl.routes.size();
  statistics.publisher_count = impl.registrations.size();
  statistics.revocation_count = impl.revocations.size();
  statistics.path_dependency_count = impl.path_index.size();
  statistics.outstanding_programming_count = impl.outstanding_programming;
  statistics.counters = impl.counters;
  std::set<RoutingNamespace> namespaces;
  std::vector<Digest128> digests;
  digests.reserve(impl.routes.size());
  for (const auto& pair : impl.routes) {
    const RouteRecord& record = pair.second;
    namespaces.insert(record.key.routing_namespace);
    digests.push_back(semantic_digest(record));
    switch (record.lifecycle) {
      case RouteLifecycle::Installed:
        ++statistics.installed_count;
        break;
      case RouteLifecycle::RevalidationRequired:
        ++statistics.revalidation_required_count;
        break;
      case RouteLifecycle::Withdrawn:
        ++statistics.withdrawn_count;
        break;
      case RouteLifecycle::Retired:
        ++statistics.retired_count;
        break;
      case RouteLifecycle::Failed:
        ++statistics.failed_count;
        break;
      case RouteLifecycle::Superseded:
        ++statistics.superseded_count;
        break;
      default:
        break;
    }
    if (is_current(impl.currentness_of(record))) {
      ++statistics.current_count;
    }
  }
  statistics.routing_namespace_count = namespaces.size();
  std::sort(digests.begin(), digests.end());
  Fnv1a64 forward(0xCBF29CE484222325ull);
  Fnv1a64 backward(0x9AE16A3B2F90404Full);
  forward.update(std::string_view("routefabric.state-digest.v1"));
  backward.update(std::string_view("routefabric.state-digest.v1"));
  for (const Digest128& digest : digests) {
    forward.update(digest.bytes());
    backward.update(digest.bytes());
  }
  ByteWriter epoch_bytes;
  impl.epoch.write(epoch_bytes);
  forward.update(epoch_bytes.buffer());
  backward.update(epoch_bytes.buffer());
  statistics.state_digest = Digest128::from_u64_pair(mix64(forward.value()), mix64(backward.value()));
  return statistics;
}

RouteCounters RouteFabricRuntime::counters() const {
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  return impl.counters;
}

Status RouteFabricRuntime::ValidateIndexes() const {
  CallGuard guard;
  if (guard.reentrant()) {
    return reentrancy_error();
  }
  const Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  std::size_t keyed_routes = 0;
  for (const auto& pair : impl.routes) {
    if (pair.second.lifecycle != RouteLifecycle::Superseded) {
      ++keyed_routes;
    }
  }
  if (impl.key_index.size() != keyed_routes) {
    return make_error(StatusCode::Internal, "the route key index size does not match the keyed route count");
  }
  std::map<PathId, std::size_t> path_counts;
  std::map<PublisherId, std::size_t> publisher_counts;
  std::map<RouteLifecycle, std::size_t> lifecycle_counts;
  for (const auto& pair : impl.routes) {
    const RouteRecord& record = pair.second;
    const auto key_it = impl.key_index.find(record.key);
    if (record.lifecycle == RouteLifecycle::Superseded) {
      if (key_it != impl.key_index.end() && key_it->second == record.id) {
        return make_error(StatusCode::Internal, "a superseded lineage still owns its route key");
      }
    } else if (key_it == impl.key_index.end() || !(key_it->second == record.id)) {
      return make_error(StatusCode::Internal, "the route key index disagrees with the route map");
    }
    if (record.binding.kind == BindingKind::AuthorizedPath) {
      const auto path_it = impl.path_index.find(record.binding.path);
      if (path_it == impl.path_index.end() || path_it->second.find(record.id) == path_it->second.end()) {
        return make_error(StatusCode::Internal, "the path dependency index disagrees with the route map");
      }
      ++path_counts[record.binding.path];
    }
    const auto publisher_it = impl.publisher_index.find(record.provenance.publisher);
    if (publisher_it == impl.publisher_index.end() ||
        publisher_it->second.find(record.id) == publisher_it->second.end()) {
      return make_error(StatusCode::Internal, "the publisher index disagrees with the route map");
    }
    ++publisher_counts[record.provenance.publisher];
    const auto lifecycle_it = impl.lifecycle_index.find(record.lifecycle);
    if (lifecycle_it == impl.lifecycle_index.end() ||
        lifecycle_it->second.find(record.id) == lifecycle_it->second.end()) {
      return make_error(StatusCode::Internal, "the lifecycle index disagrees with the route map");
    }
    ++lifecycle_counts[record.lifecycle];
  }
  std::size_t path_total = 0;
  for (const auto& pair : impl.path_index) {
    path_total += pair.second.size();
  }
  std::size_t expected_path_total = 0;
  for (const auto& pair : path_counts) {
    expected_path_total += pair.second;
  }
  if (path_total != expected_path_total) {
    return make_error(StatusCode::Internal, "the path dependency index contains stale entries");
  }
  std::size_t publisher_total = 0;
  for (const auto& pair : impl.publisher_index) {
    publisher_total += pair.second.size();
  }
  std::size_t expected_publisher_total = 0;
  for (const auto& pair : publisher_counts) {
    expected_publisher_total += pair.second;
  }
  if (publisher_total != expected_publisher_total) {
    return make_error(StatusCode::Internal, "the publisher index contains stale entries");
  }
  std::size_t lifecycle_total = 0;
  for (const auto& pair : impl.lifecycle_index) {
    lifecycle_total += pair.second.size();
  }
  std::size_t expected_lifecycle_total = 0;
  for (const auto& pair : lifecycle_counts) {
    expected_lifecycle_total += pair.second;
  }
  if (lifecycle_total != expected_lifecycle_total) {
    return make_error(StatusCode::Internal, "the lifecycle index contains stale entries");
  }
  std::map<RoutingNamespace, std::size_t> namespace_counts;
  for (const auto& pair : impl.routes) {
    ++namespace_counts[pair.second.key.routing_namespace];
  }
  if (namespace_counts.size() != impl.namespace_counts.size()) {
    return make_error(StatusCode::Internal, "the routing namespace index disagrees with the route map");
  }
  for (const auto& pair : namespace_counts) {
    const auto it = impl.namespace_counts.find(pair.first);
    if (it == impl.namespace_counts.end() || it->second != pair.second) {
      return make_error(StatusCode::Internal, "the routing namespace index contains a stale count");
    }
  }
  std::size_t unresolved = 0;
  for (const auto& pair : impl.attempts) {
    if (impl.routes.find(pair.second.route) == impl.routes.end()) {
      return make_error(StatusCode::Internal, "a programming attempt references a missing route");
    }
    if (!pair.second.resolved) {
      ++unresolved;
    }
  }
  if (unresolved != impl.outstanding_programming) {
    return make_error(StatusCode::Internal, "the outstanding programming counter disagrees with the attempt map");
  }
  return ok_status();
}

}  // namespace routefabric
