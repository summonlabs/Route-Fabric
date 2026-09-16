// Shows that an epoch advance invalidates live authority while durable intent stays.
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
  const Expected<CoordinatorEpoch> advanced = instance->runtime->AdvanceEpoch();
  if (!expect(advanced.has_value(), "epoch advance")) {
    return 1;
  }
  say("epoch " + advanced.value().render());
  const Expected<PublishResult> stale =
      instance->runtime->PublishRoute(instance->publish_request(instance->key("10.0.1.0/24"),
                                                               instance->next_hop_binding(2), 2));
  say(std::string("stale epoch rejection ") +
      (stale.has_value() ? std::string("none") : std::string(to_string(stale.error().code()))));
  const Expected<RouteSnapshot> snapshot = instance->runtime->QueryRoute(key);
  if (!expect(snapshot.has_value(), "query")) {
    return 1;
  }
  say(std::string("durable lifecycle ") + to_string(snapshot.value().record.lifecycle));
  say(std::string("currentness ") + to_string(snapshot.value().currentness));
  return (!stale.has_value() && !is_current(snapshot.value().currentness)) ? 0 : 1;
}
