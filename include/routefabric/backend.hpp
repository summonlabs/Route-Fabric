#ifndef ROUTEFABRIC_BACKEND_HPP
#define ROUTEFABRIC_BACKEND_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"
#include "routefabric/record.hpp"

namespace routefabric {

// Vendor-neutral, structured programming outcomes. There is no boolean result
// anywhere in this contract.
enum class ProgrammingOutcome : std::uint8_t {
  Applied = 1,
  Idempotent = 2,
  NotSupported = 3,
  Rejected = 4,
  RetryableFailure = 5,
  PermanentFailure = 6,
  Ambiguous = 7,
  BackendUnavailable = 8,
};

const char* to_string(ProgrammingOutcome outcome) noexcept;
bool parse_programming_outcome(std::string_view text, ProgrammingOutcome& out) noexcept;

enum class ProgrammingOperation : std::uint8_t {
  Install = 1,
  Replace = 2,
  Withdraw = 3,
};

const char* to_string(ProgrammingOperation operation) noexcept;

// Presence reported by a backend query. The runtime derives the reconciliation
// classification (MATCHED / MISSING / DIVERGED / EXTRA / UNKNOWN) from presence
// plus the desired state; the backend never decides authority.
enum class BackendPresence : std::uint8_t {
  Unknown = 1,
  Present = 2,
  Absent = 3,
  Unavailable = 4,
};

const char* to_string(BackendPresence presence) noexcept;

struct RouteProgrammingRequest {
  ProgrammingOperation operation = ProgrammingOperation::Install;
  ProgrammingAttemptId attempt;
  ProgrammingGeneration programming_generation;
  RouteId route;
  RouteKey key;
  RouteBinding binding;
  RouteGeneration desired_generation;
  RouteAuthorityGeneration authority_generation;
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId worker_boot;
  bool previously_installed = false;

  std::string render() const;
};

struct ProgrammingResult {
  ProgrammingOutcome outcome = ProgrammingOutcome::BackendUnavailable;
  ProgrammingAttemptId attempt;
  BackendId backend;
  std::string detail;
};

// A programming call is either resolved inline or explicitly deferred: a
// deferred dispatch is acknowledged as outstanding and its outcome arrives later
// through RouteFabricRuntime::ApplyProgrammingCompletion.
struct ProgrammingDispatch {
  bool deferred = false;
  ProgrammingResult immediate;
};

struct BackendQueryResult {
  BackendPresence presence = BackendPresence::Unknown;
  BackendId backend;
  RouteGeneration observed_generation;
  std::string detail;
};

struct BackendCapabilities {
  bool install = false;
  bool replace = false;
  bool withdraw = false;
  bool query = false;
  // True when the backend is physically incapable of mutating the host routing
  // table; mutation calls return NotSupported.
  bool read_only = false;
};

// Route programming backend contract.
//
// Contract:
//   * Programming and query calls are made outside the Route Fabric mutation
//     lock. id() and capabilities() are cheap accessors and may be called while
//     the lock is held; they must not block.
//   * A backend must never call back into RouteFabricRuntime from inside a
//     programming call; completions are delivered through the runtime API.
//   * A backend must never fabricate a successful application it did not
//     perform.
class IRouteProgrammingBackend {
 public:
  virtual ~IRouteProgrammingBackend() = default;

  virtual BackendId id() const = 0;
  virtual BackendCapabilities capabilities() const = 0;

  virtual ProgrammingDispatch InstallRoute(const RouteProgrammingRequest& request) = 0;
  virtual ProgrammingDispatch ReplaceRoute(const RouteProgrammingRequest& request) = 0;
  virtual ProgrammingDispatch WithdrawRoute(const RouteProgrammingRequest& request) = 0;
  virtual BackendQueryResult QueryRoute(const RouteKey& key) = 0;
};

// Deterministic synthetic programming backend. It exists to exercise every
// programming outcome exhaustively and is labelled SYNTHETIC wherever it is
// reported. It never touches the host routing table.
class SyntheticProgrammingBackend final : public IRouteProgrammingBackend {
 public:
  explicit SyntheticProgrammingBackend(BackendId id);

  BackendId id() const override;
  BackendCapabilities capabilities() const override;

  ProgrammingDispatch InstallRoute(const RouteProgrammingRequest& request) override;
  ProgrammingDispatch ReplaceRoute(const RouteProgrammingRequest& request) override;
  ProgrammingDispatch WithdrawRoute(const RouteProgrammingRequest& request) override;
  BackendQueryResult QueryRoute(const RouteKey& key) override;

  // --- Deterministic scripting -------------------------------------------
  // Queue one scripted outcome for the next mutating call.
  void EnqueueOutcome(ProgrammingOutcome outcome, bool deferred = false, std::string detail = std::string());
  // Outcome used once the queue is exhausted.
  void SetDefaultOutcome(ProgrammingOutcome outcome, bool deferred = false);
  // Presence reported by QueryRoute, plus an optional observed generation that
  // makes the observation divergent.
  void SetObservedPresence(BackendPresence presence, std::string detail = std::string());
  void SetObservedGeneration(const RouteGeneration& generation);
  void SetAvailable(bool available);

  // Completes a deferred attempt. Returns false when the attempt is unknown or
  // already completed.
  bool CompleteDeferred(const ProgrammingAttemptId& attempt, ProgrammingOutcome outcome,
                        std::string detail = std::string());
  bool HasDeferred(const ProgrammingAttemptId& attempt) const;
  std::vector<ProgrammingAttemptId> DeferredAttempts() const;

  // Recorded mutating calls, in dispatch order.
  std::vector<RouteProgrammingRequest> RecordedCalls() const;
  std::size_t call_count() const;
  void Reset();

 private:
  struct ScriptedOutcome {
    ProgrammingOutcome outcome = ProgrammingOutcome::Applied;
    bool deferred = false;
    std::string detail;
  };

  ProgrammingDispatch Dispatch(const RouteProgrammingRequest& request, bool install_family);

  mutable std::mutex mutex_;
  BackendId id_;
  std::deque<ScriptedOutcome> queue_;
  ScriptedOutcome default_outcome_;
  std::map<ProgrammingAttemptId, ProgrammingResult> deferred_;
  std::vector<RouteProgrammingRequest> calls_;
  BackendPresence observed_presence_ = BackendPresence::Absent;
  std::string observed_detail_;
  RouteGeneration observed_generation_;
  bool available_ = true;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_BACKEND_HPP
