#ifndef ROUTEFABRIC_LIFECYCLE_HPP
#define ROUTEFABRIC_LIFECYCLE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "routefabric/error.hpp"

namespace routefabric {

// Authoritative route lifecycle. The numeric values are part of the persistence
// and wire representations and must not change once released.
//
// The model is deliberately minimal while preserving every distinction that
// changes behaviour:
//   Declared              - authoritative intent record exists, nothing checked
//   Validating            - authority, scope, generation and path binding checked
//   Ready                 - installable, nothing dispatched to the backend
//   Installing            - a programming attempt is outstanding
//   Installed             - the backend reported the desired state as applied
//   RevalidationRequired  - desired state exists but applied state is unproven
//   Withdrawing           - a withdrawal attempt is outstanding
//   Withdrawn             - the backend reported the route as removed
//   Failed                - programming was definitively refused
//   Superseded            - the lineage lost the key to a successor lineage
//   Retired               - the lineage is permanently unusable
enum class RouteLifecycle : std::uint8_t {
  Declared = 1,
  Validating = 2,
  Ready = 3,
  Installing = 4,
  Installed = 5,
  RevalidationRequired = 6,
  Withdrawing = 7,
  Withdrawn = 8,
  Failed = 9,
  Superseded = 10,
  Retired = 11,
};

// Lifecycle events. Numeric values are part of the persistence format.
enum class RouteEvent : std::uint8_t {
  BeginValidation = 1,
  ValidationAccepted = 2,
  ValidationRejected = 3,
  BeginInstall = 4,
  BackendApplied = 5,
  BackendIdempotent = 6,
  BackendRejected = 7,
  BackendNotSupported = 8,
  BackendRetryableFailure = 9,
  BackendPermanentFailure = 10,
  BackendAmbiguous = 11,
  BackendUnavailable = 12,
  RequireRevalidation = 13,
  BeginWithdraw = 14,
  WithdrawApplied = 15,
  WithdrawFailed = 16,
  Supersede = 17,
  Retire = 18,
  Revoke = 19,
};

constexpr std::size_t kRouteLifecycleCount = 11;
constexpr std::size_t kRouteEventCount = 19;

const char* to_string(RouteLifecycle lifecycle) noexcept;
const char* to_string(RouteEvent event) noexcept;

bool parse_lifecycle(std::string_view text, RouteLifecycle& out) noexcept;
bool parse_route_event(std::string_view text, RouteEvent& out) noexcept;

bool is_lifecycle_value(std::uint8_t raw) noexcept;
bool is_route_event_value(std::uint8_t raw) noexcept;

// True when no event may move the route out of this state.
bool is_terminal(RouteLifecycle lifecycle) noexcept;

// True when the lifecycle alone permits the route to be CURRENT. Currentness is
// still gated by live authority, path authority and backend evidence.
bool is_current_capable(RouteLifecycle lifecycle) noexcept;

// True when a programming attempt is outstanding in this state.
bool has_outstanding_programming(RouteLifecycle lifecycle) noexcept;

struct LifecycleTransition {
  RouteLifecycle from;
  RouteEvent event;
  RouteLifecycle to;
};

// The complete transition table. Every (state, event) pair that is not listed
// here is rejected; the table is the single source of truth and is verified
// exhaustively by the test suite.
const LifecycleTransition* transition_table() noexcept;
std::size_t transition_table_size() noexcept;

// Returns the destination state, or nullopt when the transition is not allowed.
std::optional<RouteLifecycle> next_lifecycle(RouteLifecycle from, RouteEvent event) noexcept;

// Applies an event, returning a LifecycleViolation error when it is not allowed.
Status apply_event(RouteLifecycle& state, RouteEvent event);

}  // namespace routefabric

#endif  // ROUTEFABRIC_LIFECYCLE_HPP
