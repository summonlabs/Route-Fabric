// Withdraws a route and distinguishes the request from the applied outcome.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
    return 1;
  }
  const RouteKey key = instance->key("10.0.0.0/24");
  const Expected<PublishResult> published =
      instance->runtime->PublishRoute(instance->publish_request(key, instance->next_hop_binding(1)));
  if (!expect(published.has_value(), "publication")) {
    return 1;
  }
  WithdrawRequest withdraw;
  withdraw.publisher = instance->publisher;
  withdraw.worker_boot = instance->boot;
  withdraw.attempt = make_id<MutationAttemptId>(9);
  withdraw.route = published.value().route;
  withdraw.reason = "example withdrawal";
  const Expected<WithdrawOutcome> withdrawn = instance->runtime->WithdrawRoute(withdraw);
  if (!expect(withdrawn.has_value(), "withdrawal")) {
    return 1;
  }
  say(std::string("after withdrawal lifecycle ") + to_string(withdrawn.value().lifecycle));
  say(std::string("after withdrawal applied ") + to_string(withdrawn.value().applied));
  const Expected<WithdrawOutcome> repeated = instance->runtime->WithdrawRoute(withdraw);
  if (!expect(repeated.has_value(), "repeated withdrawal")) {
    return 1;
  }
  say(std::string("repeated withdrawal idempotent ") + (repeated.value().already_withdrawn ? "yes" : "no"));
  return repeated.value().already_withdrawn ? 0 : 1;
}
