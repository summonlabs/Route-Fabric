// Publishes a route bound to an exact path and an exact Path Authority generation.
#include "example_support.hpp"

int main() {
  using namespace example;
  auto instance = ExampleRuntime::Create();
  const PathId path = make_id<PathId>(77);
  const PathAuthorityGeneration generation = PathAuthorityGeneration::from_value(3);
  if (!expect(instance->path_authority.SetPath(path, generation, PathAuthorization::Usable).has_value(),
              "path authority setup")) {
    return 1;
  }
  if (!expect(instance->register_publisher(instance->publisher, instance->boot), "publisher registration")) {
    return 1;
  }
  const RouteKey key = instance->key("2001:db8:1::/48");
  const RouteBinding binding = instance->path_binding(path, generation);
  const Expected<PublishResult> published =
      instance->runtime->PublishRoute(instance->publish_request(key, binding));
  if (!expect(published.has_value(), "publication")) {
    return 1;
  }
  say(std::string("binding ") + binding.render());
  say(std::string("currentness ") + to_string(published.value().currentness));

  // Path Authority advances the generation: the route stops being current.
  const Expected<PathAuthorityGeneration> advanced =
      instance->path_authority.Invalidate(path, PathAuthorization::Usable);
  if (!expect(advanced.has_value(), "path invalidation")) {
    return 1;
  }
  if (!expect(instance->runtime->OnPathAuthorityChanged(path, advanced.value(), PathAuthorization::Usable).has_value(),
              "path change notification")) {
    return 1;
  }
  const Expected<RouteSnapshot> snapshot = instance->runtime->QueryRoute(key);
  if (!expect(snapshot.has_value(), "query")) {
    return 1;
  }
  say(std::string("after invalidation lifecycle ") + to_string(snapshot.value().record.lifecycle));
  say(std::string("after invalidation currentness ") + to_string(snapshot.value().currentness));
  return is_current(snapshot.value().currentness) ? 1 : 0;
}
