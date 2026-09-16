#include "test_framework.hpp"

#include <set>
#include <utility>

#include "routefabric/lifecycle.hpp"

using namespace routefabric;

namespace {

constexpr std::size_t kLifecycleCount = kRouteLifecycleCount;
constexpr std::size_t kEventCount = kRouteEventCount;

RouteLifecycle lifecycle_at(std::size_t index) { return static_cast<RouteLifecycle>(index + 1); }
RouteEvent event_at(std::size_t index) { return static_cast<RouteEvent>(index + 1); }

}  // namespace

RF_TEST(lifecycle_values_are_stable_and_parseable) {
  for (std::size_t index = 0; index < kLifecycleCount; ++index) {
    const RouteLifecycle lifecycle = lifecycle_at(index);
    const char* text = to_string(lifecycle);
    RF_CHECK(std::string(text) != "UNKNOWN");
    RouteLifecycle parsed = RouteLifecycle::Declared;
    RF_CHECK(parse_lifecycle(text, parsed));
    RF_CHECK(parsed == lifecycle);
    RF_CHECK(is_lifecycle_value(static_cast<std::uint8_t>(lifecycle)));
  }
  RF_CHECK(!is_lifecycle_value(0));
  RF_CHECK(!is_lifecycle_value(static_cast<std::uint8_t>(kLifecycleCount + 1)));
  RouteLifecycle parsed = RouteLifecycle::Declared;
  RF_CHECK(!parse_lifecycle("NOPE", parsed));
}

RF_TEST(event_values_are_stable_and_parseable) {
  for (std::size_t index = 0; index < kEventCount; ++index) {
    const RouteEvent event = event_at(index);
    const char* text = to_string(event);
    RF_CHECK(std::string(text) != "UNKNOWN");
    RouteEvent parsed = RouteEvent::BeginValidation;
    RF_CHECK(parse_route_event(text, parsed));
    RF_CHECK(parsed == event);
  }
  RouteEvent parsed = RouteEvent::BeginValidation;
  RF_CHECK(!parse_route_event("NOPE", parsed));
}

RF_TEST(transition_table_is_total_and_consistent) {
  const LifecycleTransition* table = transition_table();
  const std::size_t size = transition_table_size();
  RF_CHECK(size > 0);
  std::set<std::pair<std::uint8_t, std::uint8_t>> seen;
  for (std::size_t index = 0; index < size; ++index) {
    const LifecycleTransition& transition = table[index];
    RF_CHECK(is_lifecycle_value(static_cast<std::uint8_t>(transition.from)));
    RF_CHECK(is_route_event_value(static_cast<std::uint8_t>(transition.event)));
    RF_CHECK(is_lifecycle_value(static_cast<std::uint8_t>(transition.to)));
    const auto key = std::make_pair(static_cast<std::uint8_t>(transition.from),
                                    static_cast<std::uint8_t>(transition.event));
    RF_CHECK(seen.insert(key).second);  // at most one destination per (state, event)
  }
  RF_CHECK_EQ(seen.size(), size);
}

RF_TEST(every_state_event_pair_is_explicitly_allowed_or_rejected) {
  const LifecycleTransition* table = transition_table();
  const std::size_t size = transition_table_size();
  for (std::size_t state_index = 0; state_index < kLifecycleCount; ++state_index) {
    for (std::size_t event_index = 0; event_index < kEventCount; ++event_index) {
      const RouteLifecycle state = lifecycle_at(state_index);
      const RouteEvent event = event_at(event_index);
      const std::optional<RouteLifecycle> expected = [&]() -> std::optional<RouteLifecycle> {
        for (std::size_t index = 0; index < size; ++index) {
          if (table[index].from == state && table[index].event == event) {
            return table[index].to;
          }
        }
        return std::nullopt;
      }();
      const std::optional<RouteLifecycle> actual = next_lifecycle(state, event);
      RF_CHECK(expected.has_value() == actual.has_value());
      if (expected.has_value() && actual.has_value()) {
        RF_CHECK(expected.value() == actual.value());
      }
      RouteLifecycle mutable_state = state;
      const Status status = apply_event(mutable_state, event);
      if (expected.has_value()) {
        RF_CHECK(status.has_value());
        RF_CHECK(mutable_state == expected.value());
      } else {
        RF_CHECK(!status.has_value());
        RF_CHECK(status.error().code() == StatusCode::LifecycleViolation);
        RF_CHECK(mutable_state == state);  // a rejected event never mutates the state
      }
    }
  }
}

RF_TEST(retired_lifecycle_never_leaves) {
  RouteLifecycle retired = RouteLifecycle::Retired;
  for (std::size_t index = 0; index < kEventCount; ++index) {
    RouteLifecycle candidate = retired;
    const Status status = apply_event(candidate, event_at(index));
    if (status) {
      RF_CHECK(candidate == RouteLifecycle::Retired);
    }
  }
  RF_CHECK(is_terminal(RouteLifecycle::Retired));
  RF_CHECK(!is_terminal(RouteLifecycle::Installed));
  RF_CHECK(!is_terminal(RouteLifecycle::Superseded));
}

RF_TEST(lifecycle_classification_helpers) {
  RF_CHECK(is_current_capable(RouteLifecycle::Installed));
  RF_CHECK(!is_current_capable(RouteLifecycle::Installing));
  RF_CHECK(!is_current_capable(RouteLifecycle::RevalidationRequired));
  RF_CHECK(!is_current_capable(RouteLifecycle::Withdrawn));
  RF_CHECK(has_outstanding_programming(RouteLifecycle::Installing));
  RF_CHECK(has_outstanding_programming(RouteLifecycle::Withdrawing));
  RF_CHECK(!has_outstanding_programming(RouteLifecycle::Installed));
  RF_CHECK(!has_outstanding_programming(RouteLifecycle::Ready));
}

RF_TEST(ambiguous_and_unavailable_outcomes_require_revalidation) {
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendAmbiguous).value() ==
           RouteLifecycle::RevalidationRequired);
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendUnavailable).value() ==
           RouteLifecycle::RevalidationRequired);
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendRetryableFailure).value() ==
           RouteLifecycle::RevalidationRequired);
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendPermanentFailure).value() ==
           RouteLifecycle::Failed);
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendRejected).value() == RouteLifecycle::Failed);
  RF_CHECK(next_lifecycle(RouteLifecycle::Installing, RouteEvent::BackendNotSupported).value() ==
           RouteLifecycle::Failed);
  RF_CHECK(next_lifecycle(RouteLifecycle::Withdrawing, RouteEvent::WithdrawApplied).value() ==
           RouteLifecycle::Withdrawn);
  RF_CHECK(next_lifecycle(RouteLifecycle::Withdrawing, RouteEvent::WithdrawFailed).value() ==
           RouteLifecycle::RevalidationRequired);
}

RF_TEST(a_backend_outcome_never_reaches_installed_from_a_terminal_state) {
  RF_CHECK(!next_lifecycle(RouteLifecycle::Retired, RouteEvent::BackendApplied).has_value());
  RF_CHECK(!next_lifecycle(RouteLifecycle::Superseded, RouteEvent::BackendApplied).has_value());
  RF_CHECK(!next_lifecycle(RouteLifecycle::Withdrawn, RouteEvent::WithdrawApplied).has_value());
  RF_CHECK(!next_lifecycle(RouteLifecycle::Installed, RouteEvent::WithdrawApplied).has_value());
}
