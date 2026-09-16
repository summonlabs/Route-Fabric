// Shows conservative recovery and backend reconciliation across a restart.
#include "example_support.hpp"

#include <filesystem>

int main() {
  using namespace example;
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "routefabric-example-restart";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path store = directory / "fabric";

  {
    auto instance = ExampleRuntime::Create();
    RuntimeConfig config;
    config.fabric = instance->fabric;
    config.durability = Durability::Journal;
    config.store_path = store;
    auto durable = ExampleRuntime::Create(config);
    if (!expect(durable->register_publisher(durable->publisher, durable->boot), "publisher registration")) {
      return 1;
    }
    const Expected<PublishResult> published = durable->runtime->PublishRoute(
        durable->publish_request(durable->key("10.0.0.0/24"), durable->next_hop_binding(1)));
    if (!expect(published.has_value(), "publication")) {
      return 1;
    }
    say("before restart currentness " + std::string(to_string(published.value().currentness)));
  }

  // A new runtime over the same store: intent survives, live authority does not.
  auto restarted = ExampleRuntime::Create();
  RuntimeConfig config;
  config.fabric = restarted->fabric;
  config.durability = Durability::Journal;
  config.store_path = store;
  auto recovered = ExampleRuntime::Create(config, false);
  if (!expect(recovered->runtime->Open().has_value(), "recovery open")) {
    return 1;
  }
  const Expected<RouteSnapshot> snapshot = recovered->runtime->QueryRoute(recovered->key("10.0.0.0/24"));
  if (!expect(snapshot.has_value(), "recovered route")) {
    return 1;
  }
  say(std::string("after restart lifecycle ") + to_string(snapshot.value().record.lifecycle));
  say(std::string("after restart currentness ") + to_string(snapshot.value().currentness));

  // Reconciliation records evidence: the synthetic backend lost its process-local
  // memory, so the route is reported missing rather than fabricated as installed.
  const Expected<ObservationClass> classification = recovered->runtime->ReconcileRoute(snapshot.value().record.id);
  if (!expect(classification.has_value(), "reconciliation")) {
    return 1;
  }
  say(std::string("reconciliation ") + to_string(classification.value()));

  // Re-establishing authority requires explicit registration and revalidation.
  if (!expect(recovered->register_publisher(recovered->publisher, make_id<WorkerBootId>(2)),
              "re-registration")) {
    return 1;
  }
  RevalidateRequest revalidate;
  revalidate.publisher = recovered->publisher;
  revalidate.worker_boot = make_id<WorkerBootId>(2);
  revalidate.attempt = make_id<MutationAttemptId>(5);
  revalidate.route = snapshot.value().record.id;
  revalidate.reason = "post-restart revalidation";
  const Expected<WithdrawOutcome> revalidated = recovered->runtime->RevalidateRoute(revalidate);
  if (!expect(revalidated.has_value(), "revalidation")) {
    return 1;
  }
  say(std::string("after revalidation lifecycle ") + to_string(revalidated.value().lifecycle));
  std::filesystem::remove_all(directory, error);
  return revalidated.value().lifecycle == RouteLifecycle::Installed ? 0 : 1;
}
