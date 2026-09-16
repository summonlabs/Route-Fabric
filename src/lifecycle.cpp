#include "routefabric/lifecycle.hpp"


namespace routefabric {
namespace {

// clang-format off
constexpr LifecycleTransition kTransitions[] = {
  // Declared
  {RouteLifecycle::Declared, RouteEvent::BeginValidation, RouteLifecycle::Validating},
  {RouteLifecycle::Declared, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Declared, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Declared, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Validating
  {RouteLifecycle::Validating, RouteEvent::ValidationAccepted, RouteLifecycle::Ready},
  {RouteLifecycle::Validating, RouteEvent::ValidationRejected, RouteLifecycle::Failed},
  {RouteLifecycle::Validating, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Validating, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Validating, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Validating, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Ready
  {RouteLifecycle::Ready, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Ready, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Ready, RouteEvent::BeginWithdraw, RouteLifecycle::Withdrawing},
  {RouteLifecycle::Ready, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Ready, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Ready, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Installing
  {RouteLifecycle::Installing, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Installing, RouteEvent::BackendApplied, RouteLifecycle::Installed},
  {RouteLifecycle::Installing, RouteEvent::BackendIdempotent, RouteLifecycle::Installed},
  {RouteLifecycle::Installing, RouteEvent::BackendRejected, RouteLifecycle::Failed},
  {RouteLifecycle::Installing, RouteEvent::BackendNotSupported, RouteLifecycle::Failed},
  {RouteLifecycle::Installing, RouteEvent::BackendPermanentFailure, RouteLifecycle::Failed},
  {RouteLifecycle::Installing, RouteEvent::BackendRetryableFailure, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Installing, RouteEvent::BackendAmbiguous, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Installing, RouteEvent::BackendUnavailable, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Installing, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Installing, RouteEvent::BeginWithdraw, RouteLifecycle::Withdrawing},
  {RouteLifecycle::Installing, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Installing, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Installing, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Installed
  {RouteLifecycle::Installed, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Installed, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Installed, RouteEvent::BeginWithdraw, RouteLifecycle::Withdrawing},
  {RouteLifecycle::Installed, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Installed, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Installed, RouteEvent::Revoke, RouteLifecycle::Retired},

  // RevalidationRequired
  {RouteLifecycle::RevalidationRequired, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::RevalidationRequired, RouteEvent::ValidationRejected, RouteLifecycle::Failed},
  {RouteLifecycle::RevalidationRequired, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::RevalidationRequired, RouteEvent::BeginWithdraw, RouteLifecycle::Withdrawing},
  {RouteLifecycle::RevalidationRequired, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::RevalidationRequired, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::RevalidationRequired, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Withdrawing
  {RouteLifecycle::Withdrawing, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Withdrawing, RouteEvent::WithdrawApplied, RouteLifecycle::Withdrawn},
  {RouteLifecycle::Withdrawing, RouteEvent::WithdrawFailed, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Withdrawing, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Withdrawing, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Withdrawing, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Withdrawing, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Withdrawn
  {RouteLifecycle::Withdrawn, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Withdrawn, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Withdrawn, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Withdrawn, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Withdrawn, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Failed
  {RouteLifecycle::Failed, RouteEvent::BeginInstall, RouteLifecycle::Installing},
  {RouteLifecycle::Failed, RouteEvent::RequireRevalidation, RouteLifecycle::RevalidationRequired},
  {RouteLifecycle::Failed, RouteEvent::BeginWithdraw, RouteLifecycle::Withdrawing},
  {RouteLifecycle::Failed, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Failed, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Failed, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Superseded
  {RouteLifecycle::Superseded, RouteEvent::Supersede, RouteLifecycle::Superseded},
  {RouteLifecycle::Superseded, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Superseded, RouteEvent::Revoke, RouteLifecycle::Retired},

  // Retired
  {RouteLifecycle::Retired, RouteEvent::Retire, RouteLifecycle::Retired},
  {RouteLifecycle::Retired, RouteEvent::Revoke, RouteLifecycle::Retired},
  {RouteLifecycle::Retired, RouteEvent::Supersede, RouteLifecycle::Retired},
  {RouteLifecycle::Retired, RouteEvent::RequireRevalidation, RouteLifecycle::Retired},
};
// clang-format on

}  // namespace

const char* to_string(RouteLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RouteLifecycle::Declared:
      return "DECLARED";
    case RouteLifecycle::Validating:
      return "VALIDATING";
    case RouteLifecycle::Ready:
      return "READY";
    case RouteLifecycle::Installing:
      return "INSTALLING";
    case RouteLifecycle::Installed:
      return "INSTALLED";
    case RouteLifecycle::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case RouteLifecycle::Withdrawing:
      return "WITHDRAWING";
    case RouteLifecycle::Withdrawn:
      return "WITHDRAWN";
    case RouteLifecycle::Failed:
      return "FAILED";
    case RouteLifecycle::Superseded:
      return "SUPERSEDED";
    case RouteLifecycle::Retired:
      return "RETIRED";
  }
  return "UNKNOWN";
}

const char* to_string(RouteEvent event) noexcept {
  switch (event) {
    case RouteEvent::BeginValidation:
      return "BEGIN_VALIDATION";
    case RouteEvent::ValidationAccepted:
      return "VALIDATION_ACCEPTED";
    case RouteEvent::ValidationRejected:
      return "VALIDATION_REJECTED";
    case RouteEvent::BeginInstall:
      return "BEGIN_INSTALL";
    case RouteEvent::BackendApplied:
      return "BACKEND_APPLIED";
    case RouteEvent::BackendIdempotent:
      return "BACKEND_IDEMPOTENT";
    case RouteEvent::BackendRejected:
      return "BACKEND_REJECTED";
    case RouteEvent::BackendNotSupported:
      return "BACKEND_NOT_SUPPORTED";
    case RouteEvent::BackendRetryableFailure:
      return "BACKEND_RETRYABLE_FAILURE";
    case RouteEvent::BackendPermanentFailure:
      return "BACKEND_PERMANENT_FAILURE";
    case RouteEvent::BackendAmbiguous:
      return "BACKEND_AMBIGUOUS";
    case RouteEvent::BackendUnavailable:
      return "BACKEND_UNAVAILABLE";
    case RouteEvent::RequireRevalidation:
      return "REQUIRE_REVALIDATION";
    case RouteEvent::BeginWithdraw:
      return "BEGIN_WITHDRAW";
    case RouteEvent::WithdrawApplied:
      return "WITHDRAW_APPLIED";
    case RouteEvent::WithdrawFailed:
      return "WITHDRAW_FAILED";
    case RouteEvent::Supersede:
      return "SUPERSEDE";
    case RouteEvent::Retire:
      return "RETIRE";
    case RouteEvent::Revoke:
      return "REVOKE";
  }
  return "UNKNOWN";
}

bool parse_lifecycle(std::string_view text, RouteLifecycle& out) noexcept {
  for (std::uint8_t raw = 1; raw <= static_cast<std::uint8_t>(kRouteLifecycleCount); ++raw) {
    const auto candidate = static_cast<RouteLifecycle>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool parse_route_event(std::string_view text, RouteEvent& out) noexcept {
  for (std::uint8_t raw = 1; raw <= static_cast<std::uint8_t>(kRouteEventCount); ++raw) {
    const auto candidate = static_cast<RouteEvent>(raw);
    if (text == to_string(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool is_lifecycle_value(std::uint8_t raw) noexcept {
  return raw >= 1 && raw <= static_cast<std::uint8_t>(kRouteLifecycleCount);
}

bool is_route_event_value(std::uint8_t raw) noexcept {
  return raw >= 1 && raw <= static_cast<std::uint8_t>(kRouteEventCount);
}

bool is_terminal(RouteLifecycle lifecycle) noexcept {
  return lifecycle == RouteLifecycle::Retired;
}

bool is_current_capable(RouteLifecycle lifecycle) noexcept {
  return lifecycle == RouteLifecycle::Installed;
}

bool has_outstanding_programming(RouteLifecycle lifecycle) noexcept {
  return lifecycle == RouteLifecycle::Installing || lifecycle == RouteLifecycle::Withdrawing;
}

const LifecycleTransition* transition_table() noexcept { return kTransitions; }

std::size_t transition_table_size() noexcept { return sizeof(kTransitions) / sizeof(kTransitions[0]); }

std::optional<RouteLifecycle> next_lifecycle(RouteLifecycle from, RouteEvent event) noexcept {
  for (const LifecycleTransition& transition : kTransitions) {
    if (transition.from == from && transition.event == event) {
      return transition.to;
    }
  }
  return std::nullopt;
}

Status apply_event(RouteLifecycle& state, RouteEvent event) {
  const std::optional<RouteLifecycle> next = next_lifecycle(state, event);
  if (!next.has_value()) {
    return make_error(StatusCode::LifecycleViolation, std::string("event ") + to_string(event) +
                                                         " is not allowed in lifecycle state " + to_string(state));
  }
  state = *next;
  return ok_status();
}

}  // namespace routefabric
