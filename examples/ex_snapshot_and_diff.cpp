// Takes two immutable snapshots and prints the deterministic diff.
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
  const Expected<RouteSnapshotSet> before = instance->runtime->Snapshot();
  if (!expect(before.has_value(), "first snapshot")) {
    return 1;
  }
  if (!expect(instance->runtime->PublishRoute(instance->publish_request(key, instance->next_hop_binding(2), 2))
                  .has_value(),
              "replacement")) {
    return 1;
  }
  const Expected<RouteSnapshotSet> after = instance->runtime->Snapshot();
  if (!expect(after.has_value(), "second snapshot")) {
    return 1;
  }
  say("snapshot-id-before " + before.value().id.render());
  say("snapshot-id-after " + after.value().id.render());
  say("digest-before " + before.value().digest.to_hex());
  say("digest-after " + after.value().digest.to_hex());
  const std::vector<RouteDiffEntry> diff =
      instance->runtime->DiffSnapshots(before.value(), after.value(), published.value().route);
  say(render_diff(published.value().route, diff));
  return diff.empty() ? 1 : 0;
}
