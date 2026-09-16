// Replaces a route and shows the preserved lineage.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
    return 1;
  }
  const RouteKey key = instance->key("10.0.0.0/24");
  const Expected<PublishResult> first =
      instance->runtime->PublishRoute(instance->publish_request(key, instance->next_hop_binding(1), 1));
  if (!expect(first.has_value(), "first publication")) {
    return 1;
  }
  const Expected<PublishResult> second =
      instance->runtime->PublishRoute(instance->publish_request(key, instance->next_hop_binding(2), 2));
  if (!expect(second.has_value(), "replacement")) {
    return 1;
  }
  say(std::string("route identity stable ") +
      (first.value().route.render() == second.value().route.render() ? "yes" : "no"));
  say("generation " + first.value().generation.render() + " -> " + second.value().generation.render());
  const Expected<RouteSnapshot> snapshot = instance->runtime->QueryRoute(key);
  if (!expect(snapshot.has_value(), "query")) {
    return 1;
  }
  say(std::string("supersession ") + to_string(snapshot.value().record.supersession.kind));
  say("prior generation " + snapshot.value().record.supersession.prior_generation.render());
  say("successor generation " + snapshot.value().record.supersession.successor_generation.render());
  return 0;
}
