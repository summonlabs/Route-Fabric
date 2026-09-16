// Shows a deferred programming dispatch whose outcome is unknown until it arrives.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
    return 1;
  }
  // The backend accepts the work but cannot resolve it synchronously.
  instance->backend.EnqueueOutcome(ProgrammingOutcome::Applied, true);
  const RouteKey key = instance->key("10.0.0.0/24");
  const Expected<PublishResult> published =
      instance->runtime->PublishRoute(instance->publish_request(key, instance->next_hop_binding(1)));
  if (!expect(published.has_value(), "publication")) {
    return 1;
  }
  say(std::string("immediately after dispatch lifecycle ") + to_string(published.value().lifecycle));
  say(std::string("immediately after dispatch applied ") + to_string(published.value().applied));
  const std::vector<ProgrammingAttemptId> attempts = instance->backend.DeferredAttempts();
  if (!expect(attempts.size() == 1, "one deferred attempt")) {
    return 1;
  }
  // The outcome arrives later; whether it is applied depends on the runtime, not
  // on the backend's opinion.
  const Expected<CompletionDisposition> disposition = instance->runtime->ApplyProgrammingCompletion(
      ProgrammingCompletion{attempts[0], ProgrammingOutcome::Applied, "late acknowledgment"});
  if (!expect(disposition.has_value(), "completion")) {
    return 1;
  }
  say(std::string("completion disposition ") + to_string(disposition.value()));
  const Expected<RouteSnapshot> snapshot = instance->runtime->QueryRoute(key);
  if (!expect(snapshot.has_value(), "query")) {
    return 1;
  }
  say(std::string("final lifecycle ") + to_string(snapshot.value().record.lifecycle));
  say(std::string("final applied ") + to_string(snapshot.value().record.applied.classification));
  return snapshot.value().record.lifecycle == RouteLifecycle::Installed ? 0 : 1;
}
