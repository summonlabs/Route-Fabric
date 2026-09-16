// Shows every structured backend refusal and how it maps onto the lifecycle.
#include "example_support.hpp"

int main() {
  using namespace example;
  const std::pair<ProgrammingOutcome, RouteLifecycle> cases[] = {
      {ProgrammingOutcome::Rejected, RouteLifecycle::Failed},
      {ProgrammingOutcome::NotSupported, RouteLifecycle::Failed},
      {ProgrammingOutcome::PermanentFailure, RouteLifecycle::Failed},
      {ProgrammingOutcome::RetryableFailure, RouteLifecycle::RevalidationRequired},
      {ProgrammingOutcome::Ambiguous, RouteLifecycle::RevalidationRequired},
      {ProgrammingOutcome::BackendUnavailable, RouteLifecycle::RevalidationRequired},
  };
  int index = 0;
  for (const auto& entry : cases) {
    ++index;
    auto instance = ExampleRuntime::Create();
    if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
      return 1;
    }
    instance->backend.EnqueueOutcome(entry.first);
    const Expected<PublishResult> published = instance->runtime->PublishRoute(instance->publish_request(
        instance->key("10.0." + to_decimal(static_cast<std::uint64_t>(index)) + ".0/24"),
        instance->next_hop_binding(static_cast<std::uint64_t>(index))));
    if (!expect(published.has_value(), "publication")) {
      return 1;
    }
    say(std::string("outcome ") + to_string(entry.first) + " -> applied " +
        to_string(published.value().applied) + " lifecycle " + to_string(published.value().lifecycle));
    if (published.value().lifecycle != entry.second) {
      say("unexpected lifecycle mapping");
      return 1;
    }
  }
  return 0;
}
