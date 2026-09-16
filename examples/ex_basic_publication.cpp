// Publishes one route and inspects the authoritative record.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
    return 1;
  }
  const RouteKey key = instance->key("10.0.0.0/24");
  PublishRequest request = instance->publish_request(key, instance->next_hop_binding(1));
  const Expected<PublishResult> published = instance->runtime->PublishRoute(request);
  if (!expect(published.has_value(), "publication")) {
    return 1;
  }
  say(std::string("backend SYNTHETIC ") + instance->backend.id().render());
  say(std::string("route ") + published.value().route.render());
  say(std::string("lifecycle ") + to_string(published.value().lifecycle));
  say(std::string("applied ") + to_string(published.value().applied));
  say(std::string("currentness ") + to_string(published.value().currentness));
  const Expected<RouteExplanation> explanation = instance->runtime->ExplainRoute(key);
  if (!expect(explanation.has_value(), "explanation")) {
    return 1;
  }
  say(std::string("authoritative ") + (explanation.value().authoritative ? "yes" : "no"));
  say(explanation.value().authority_reason);
  return 0;
}
