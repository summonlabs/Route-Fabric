// Shows that a fenced worker boot can never mutate a route again.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  const WorkerBootId first_boot = make_id<WorkerBootId>(1);
  const WorkerBootId second_boot = make_id<WorkerBootId>(2);
  if (!expect(instance->register_publisher(instance->publisher, first_boot), "first registration")) {
    return 1;
  }
  const RouteKey key = instance->key("10.0.0.0/24");
  PublishRequest request = instance->publish_request(key, instance->next_hop_binding(1));
  request.worker_boot = first_boot;
  if (!expect(instance->runtime->PublishRoute(request).has_value(), "first publication")) {
    return 1;
  }
  // The worker restarts with a fresh boot identity.
  if (!expect(instance->register_publisher(instance->publisher, second_boot), "second registration")) {
    return 1;
  }
  request.attempt = make_id<MutationAttemptId>(2);
  const Expected<PublishResult> stale = instance->runtime->PublishRoute(request);
  say(std::string("stale boot rejection ") +
      (stale.has_value() ? std::string("none") : std::string(to_string(stale.error().code()))));
  if (!expect(!stale.has_value(), "a stale boot must be rejected")) {
    return 1;
  }
  request.worker_boot = second_boot;
  request.attempt = make_id<MutationAttemptId>(3);
  if (!expect(instance->runtime->PublishRoute(request).has_value(), "fresh boot publication")) {
    return 1;
  }
  say("fresh boot publication accepted");
  return 0;
}
