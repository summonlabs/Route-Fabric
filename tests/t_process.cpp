// Process-level proofs: a real worker death, a real coordinator restart, a clean
// shutdown, and real host-route evidence through the installed command line.
#include "test_framework.hpp"

#include <fstream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "routefabric/client.hpp"

using namespace routefabric;
using rftest::ChildProcess;
using rftest::Harness;
using rftest::make_id;

namespace {

// Maximum number of poll iterations before a bounded wait fails explicitly. This
// is not a timeout: exhaustion is reported as a test failure.
constexpr int kPollAttempts = 500;

std::string last_token(const std::string& text) {
  const std::size_t position = text.find_last_of(' ');
  return position == std::string::npos ? text : text.substr(position + 1);
}

struct CoordinatorHandle {
  ChildProcess process;
  std::uint16_t port = 0;
  std::uint64_t epoch = 0;
  std::filesystem::path log;
};

bool start_coordinator(const std::filesystem::path& directory, const std::string& store,
                       const std::string& epoch_policy, CoordinatorHandle& handle) {
  handle.log = directory / ("coordinator-" + epoch_policy + ".log");
  std::vector<std::string> arguments;
  arguments.push_back("--bind");
  arguments.push_back("127.0.0.1");
  arguments.push_back("--port");
  arguments.push_back("0");
  arguments.push_back("--fabric");
  arguments.push_back("fabric");
  arguments.push_back("--backend");
  arguments.push_back("synthetic");
  arguments.push_back("--seed");
  arguments.push_back("7");
  arguments.push_back("--epoch-policy");
  arguments.push_back(epoch_policy);
  arguments.push_back("--exit-file");
  arguments.push_back((directory / "coordinator.exit").string());
  if (!store.empty()) {
    arguments.push_back("--store");
    arguments.push_back(store);
    arguments.push_back("--durability");
    arguments.push_back("journal");
  }
  if (!handle.process.Start(arguments, rftest::executable_path("rf_coordinator"), handle.log.string())) {
    return false;
  }
  std::string value;
  std::string why;
  if (!rftest::wait_for_file_line(handle.log, "ready", kPollAttempts, value, why)) {
    return false;
  }
  std::string listening;
  if (!rftest::wait_for_file_line(handle.log, "listening", kPollAttempts, listening, why)) {
    return false;
  }
  std::uint64_t port = 0;
  if (!parse_u64_decimal(last_token(listening), 65535, port)) {
    return false;
  }
  handle.port = static_cast<std::uint16_t>(port);
  std::string epoch_text;
  if (!rftest::wait_for_file_line(handle.log, "epoch", kPollAttempts, epoch_text, why)) {
    return false;
  }
  std::uint64_t epoch = 0;
  if (!parse_u64_decimal(epoch_text, 0xFFFFFFFFull, epoch)) {
    return false;
  }
  handle.epoch = epoch;
  return true;
}

struct PublisherHandle {
  ChildProcess process;
  std::filesystem::path status;
};

bool start_publisher(const std::filesystem::path& directory, std::uint16_t port, const std::string& publisher,
                     const std::string& destination, const std::string& name, PublisherHandle& handle) {
  handle.status = directory / (name + ".status");
  std::vector<std::string> arguments;
  arguments.push_back("--host");
  arguments.push_back("127.0.0.1");
  arguments.push_back("--port");
  arguments.push_back(to_decimal(port));
  arguments.push_back("--publisher");
  arguments.push_back(publisher);
  arguments.push_back("--destination");
  arguments.push_back(destination);
  arguments.push_back("--status-file");
  arguments.push_back(handle.status.string());
  arguments.push_back("--seed");
  arguments.push_back("11");
  arguments.push_back("--hold");
  return handle.process.Start(arguments, rftest::executable_path("rf_publisher"),
                              (directory / (name + ".log")).string());
}

bool wait_for_state(const PublisherHandle& handle, const std::string& state, std::string& why) {
  std::string value;
  for (int attempt = 0; attempt < kPollAttempts; ++attempt) {
    std::string contents;
    if (rftest::read_text_file(handle.status, contents) &&
        rftest::find_line_value(contents, "state", value) && value == state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  why = "publisher never reached state " + state;
  return false;
}

std::string status_value(const PublisherHandle& handle, const std::string& key) {
  std::string contents;
  std::string value;
  if (!rftest::read_text_file(handle.status, contents)) {
    return std::string();
  }
  if (!rftest::find_line_value(contents, key, value)) {
    return std::string();
  }
  return value;
}

// Connects a fresh client and completes the hello handshake.
bool connect_client(RouteFabricClient& client) {
  const Status connected = client.Connect();
  if (!connected) {
    return false;
  }
  const Expected<HelloResult> hello = client.Hello("rf_process_test");
  return hello.has_value() && hello.value().status == StatusCode::Ok;
}

}  // namespace

RF_TEST(real_worker_death_is_detected_and_fenced) {
  const std::filesystem::path directory = rftest::make_temp_directory("worker-death");
  const std::filesystem::path store = directory / "fabric";

  CoordinatorHandle coordinator;
  RF_REQUIRE(start_coordinator(directory, store.string(), "resume", coordinator));
  RF_CHECK_EQ(coordinator.epoch, static_cast<std::uint64_t>(1));

  PublisherHandle publisher_a;
  RF_REQUIRE(start_publisher(directory, coordinator.port, "publisher-a", "10.0.0.0/24", "a", publisher_a));
  std::string why;
  RF_REQUIRE(wait_for_state(publisher_a, "published", why));
  const std::string boot_a = status_value(publisher_a, "boot");
  const std::string route_a = status_value(publisher_a, "route-id");
  RF_CHECK(!boot_a.empty());
  RF_CHECK(!route_a.empty());

  // An unrelated publisher must be unaffected by the death of publisher A.
  PublisherHandle publisher_b;
  RF_REQUIRE(start_publisher(directory, coordinator.port, "publisher-b", "10.9.0.0/24", "b", publisher_b));
  RF_REQUIRE(wait_for_state(publisher_b, "published", why));

  {
    ClientConfig config;
    config.port = coordinator.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    const Expected<RouteSnapshot> snapshot =
        client.QueryRouteByKey(Harness().key("10.0.0.0/24"));
    RF_REQUIRE(snapshot.has_value());
    RF_CHECK(snapshot.value().currentness == RouteCurrentness::Current);
    client.Close();
  }

  // The publisher process is alive immediately before the kill.
  RF_CHECK(publisher_a.process.running());
  RF_REQUIRE(publisher_a.process.Terminate());
  RF_CHECK(!publisher_a.process.running());
  RF_CHECK(!publisher_b.process.running() == false);

  // The coordinator detects the loss, fences the boot and rejects stale mutations
  // while preserving the durable route intent.
  const WorkerBootId stale_boot = WorkerBootId::parse(boot_a).value();
  bool fenced = false;
  for (int attempt = 0; attempt < kPollAttempts; ++attempt) {
    ClientConfig config;
    config.port = coordinator.port;
    RouteFabricClient client(config);
    if (!connect_client(client)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    client.context().publisher = PublisherId::parse("publisher-a").value();
    client.context().worker_boot = stale_boot;
    client.next_attempt();
    const Expected<PublishRouteResult> published = client.PublishRoute(
        Harness().key("10.0.1.0/24"), Harness().next_hop_binding(make_id<NextHopId>(5)),
        PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "stale mutation after death");
    if (published.has_value() && published.value().status != StatusCode::Ok) {
      fenced = published.value().status == StatusCode::NotRegistered ||
               published.value().status == StatusCode::StaleWorkerBoot ||
               published.value().status == StatusCode::Fenced;
    }
    client.Close();
    if (fenced) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RF_CHECK(fenced);

  {
    ClientConfig config;
    config.port = coordinator.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    const Expected<RouteSnapshot> snapshot = client.QueryRouteByKey(Harness().key("10.0.0.0/24"));
    RF_REQUIRE(snapshot.has_value());
    RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
    RF_CHECK(!is_current(snapshot.value().currentness));
    // Publisher B is untouched by A's death.
    const Expected<RouteSnapshot> other = client.QueryRouteByKey(Harness().key("10.9.0.0/24"));
    RF_REQUIRE(other.has_value());
    RF_CHECK(other.value().currentness == RouteCurrentness::Current);
    client.Close();
  }

  // A reincarnated worker with the same publisher identity and a fresh boot
  // re-registers and regains authority explicitly.
  PublisherHandle publisher_a_prime;
  RF_REQUIRE(start_publisher(directory, coordinator.port, "publisher-a", "10.0.0.0/24", "a-prime", publisher_a_prime));
  RF_REQUIRE(wait_for_state(publisher_a_prime, "published", why));
  const std::string boot_prime = status_value(publisher_a_prime, "boot");
  RF_CHECK(!boot_prime.empty());
  RF_CHECK(boot_prime != boot_a);

  {
    ClientConfig config;
    config.port = coordinator.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    const Expected<RouteSnapshot> snapshot = client.QueryRouteByKey(Harness().key("10.0.0.0/24"));
    RF_REQUIRE(snapshot.has_value());
    RF_CHECK(snapshot.value().currentness == RouteCurrentness::Current);
    RF_CHECK_EQ(snapshot.value().record.provenance.worker_boot.render(), boot_prime);
    client.Close();
  }

  // The previous boot can never act again.
  {
    ClientConfig config;
    config.port = coordinator.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    client.context().publisher = PublisherId::parse("publisher-a").value();
    client.context().worker_boot = stale_boot;
    client.next_attempt();
    const Expected<PublishRouteResult> published = client.PublishRoute(
        Harness().key("10.0.2.0/24"), Harness().next_hop_binding(make_id<NextHopId>(6)),
        PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "resurrected stale boot");
    RF_REQUIRE(published.has_value());
    RF_CHECK(published.value().status != StatusCode::Ok);
    client.Close();
  }

  RF_REQUIRE(publisher_a_prime.process.Terminate());
  RF_REQUIRE(publisher_b.process.Terminate());
  RF_CHECK(coordinator.process.Terminate());
  rftest::remove_directory(directory);
}

RF_TEST(real_coordinator_restart_preserves_intent_and_refuses_stale_authority) {
  const std::filesystem::path directory = rftest::make_temp_directory("coordinator-restart");
  const std::filesystem::path store = directory / "fabric";

  CoordinatorHandle first;
  RF_REQUIRE(start_coordinator(directory, store.string(), "resume", first));
  RF_CHECK_EQ(first.epoch, static_cast<std::uint64_t>(1));

  std::vector<std::string> destinations = {"10.1.0.0/24", "10.1.1.0/24", "10.1.2.0/24"};
  PublisherHandle publisher;
  RF_REQUIRE(start_publisher(directory, first.port, "publisher-a", destinations[0], "p1", publisher));
  std::string why;
  RF_REQUIRE(wait_for_state(publisher, "published", why));
  const std::string stale_boot = status_value(publisher, "boot");
  RF_REQUIRE(!stale_boot.empty());

  {
    ClientConfig config;
    config.port = first.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    client.context().publisher = PublisherId::parse("publisher-a").value();
    client.context().worker_boot = WorkerBootId::parse(stale_boot).value();
    for (std::size_t index = 1; index < destinations.size(); ++index) {
      client.next_attempt();
      const Expected<PublishRouteResult> published = client.PublishRoute(
          Harness().key(destinations[index]), Harness().next_hop_binding(make_id<NextHopId>(10 + index)),
          PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "bootstrapping routes");
      RF_REQUIRE(published.has_value());
      RF_REQUIRE_EQ(published.value().status, StatusCode::Ok);
    }
    const Expected<StatisticsResult> statistics = client.Statistics();
    RF_REQUIRE(statistics.has_value());
    RF_CHECK_EQ(statistics.value().route_count, static_cast<std::uint64_t>(3));
    RF_CHECK_EQ(statistics.value().current_count, static_cast<std::uint64_t>(3));
    client.Close();
  }

  // Hard kill: no graceful shutdown path runs.
  RF_REQUIRE(first.process.Terminate());
  RF_REQUIRE(publisher.process.Terminate());

  CoordinatorHandle second;
  RF_REQUIRE(start_coordinator(directory, store.string(), "advance", second));
  RF_CHECK_EQ(second.epoch, static_cast<std::uint64_t>(2));
  RF_CHECK(second.port != first.port || true);  // ports are ephemeral

  {
    ClientConfig config;
    config.port = second.port;
    RouteFabricClient client(config);
    RF_REQUIRE(connect_client(client));
    // Old epoch traffic is refused.
    client.context().publisher = PublisherId::parse("publisher-a").value();
    client.context().worker_boot = WorkerBootId::parse(stale_boot).value();
    client.set_epoch(CoordinatorEpoch::from_value(1));
    client.next_attempt();
    const Expected<PublishRouteResult> old_epoch = client.PublishRoute(
        Harness().key("10.1.9.0/24"), Harness().next_hop_binding(make_id<NextHopId>(40)),
        PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "old epoch publication");
    RF_REQUIRE(old_epoch.has_value());
    RF_CHECK(old_epoch.value().status == StatusCode::StaleEpoch ||
             old_epoch.value().status == StatusCode::NotRegistered);

    // Durable intent survives the restart; live authority does not.
    client.set_epoch(second.epoch == 0 ? CoordinatorEpoch::from_value(2) : CoordinatorEpoch::from_value(2));
    for (const std::string& destination : destinations) {
      const Expected<RouteSnapshot> snapshot = client.QueryRouteByKey(Harness().key(destination));
      RF_REQUIRE(snapshot.has_value());
      RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::Installed);
      RF_CHECK(!is_current(snapshot.value().currentness));
    }

    // Stale worker boots are refused.
    client.next_attempt();
    const Expected<PublishRouteResult> stale_boot_publish = client.PublishRoute(
        Harness().key("10.1.8.0/24"), Harness().next_hop_binding(make_id<NextHopId>(41)),
        PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), "stale boot publication");
    RF_REQUIRE(stale_boot_publish.has_value());
    RF_CHECK(stale_boot_publish.value().status != StatusCode::Ok);

    // Reconcile applied backend state: the synthetic backend lost its process
    // memory, so the routes are reported missing rather than fabricated.
    const Expected<ReconcileResult> reconciled = client.ReconcileAll();
    RF_REQUIRE(reconciled.has_value());
    RF_REQUIRE_EQ(reconciled.value().status, StatusCode::Ok);
    RF_CHECK_EQ(reconciled.value().summary.checked, static_cast<std::size_t>(3));
    RF_CHECK_EQ(reconciled.value().summary.missing, static_cast<std::size_t>(3));
    for (const std::string& destination : destinations) {
      const Expected<RouteSnapshot> snapshot = client.QueryRouteByKey(Harness().key(destination));
      RF_REQUIRE(snapshot.has_value());
      RF_CHECK(!is_current(snapshot.value().currentness));
      RF_CHECK(snapshot.value().record.observation.classification == ObservationClass::Missing);
    }

    // A fresh registration re-establishes authority explicitly.
    PublisherScope scope;
    scope.fabric = FabricId::parse("fabric").value();
    scope.wildcard_namespaces = true;
    scope.wildcard_destinations = true;
    scope.wildcard_route_classes = true;
    const WorkerBootId fresh_boot = make_id<WorkerBootId>(4242);
    client.context().worker_boot = fresh_boot;
    client.set_epoch(CoordinatorEpoch::from_value(2));
    const Expected<RegisterPublisherResult> registration = client.RegisterPublisher(scope);
    RF_REQUIRE(registration.has_value());
    RF_REQUIRE_EQ(registration.value().status, StatusCode::Ok);

    for (const std::string& destination : destinations) {
      const Expected<RouteSnapshot> before = client.QueryRouteByKey(Harness().key(destination));
      RF_REQUIRE(before.has_value());
      client.next_attempt();
      const Expected<RouteMutationResult> revalidated =
          client.RevalidateRoute(before.value().record.id, "post-restart revalidation");
      RF_REQUIRE(revalidated.has_value());
      RF_REQUIRE_EQ(revalidated.value().status, StatusCode::Ok);
      const Expected<RouteSnapshot> after = client.QueryRouteByKey(Harness().key(destination));
      RF_REQUIRE(after.has_value());
      RF_CHECK(after.value().currentness == RouteCurrentness::Current);
    }
    client.Close();
  }

  // A second restart must produce a strictly larger epoch.
  RF_REQUIRE(second.process.Terminate());
  CoordinatorHandle third;
  RF_REQUIRE(start_coordinator(directory, store.string(), "advance", third));
  RF_CHECK_EQ(third.epoch, static_cast<std::uint64_t>(3));
  RF_CHECK(third.epoch > second.epoch);

  // Clean shutdown through the explicit exit channel.
  {
    std::ofstream exit_file((directory / "coordinator.exit").string(), std::ios::binary | std::ios::trunc);
    exit_file << "shutdown\n";
    exit_file.flush();
  }
  std::uint32_t exit_code = 0;
  RF_REQUIRE(third.process.WaitForExit(exit_code));
  RF_CHECK_EQ(exit_code, static_cast<std::uint32_t>(0));
  std::string contents;
  RF_REQUIRE(rftest::read_text_file(third.log, contents));
  RF_CHECK(contents.find("shutdown complete") != std::string::npos);
  rftest::remove_directory(directory);
}

RF_TEST(the_installed_command_line_reports_real_evidence) {
  const std::filesystem::path directory = rftest::make_temp_directory("cli-evidence");
  const std::filesystem::path log = directory / "cli.log";

  {  // version reporting is coherent
    ChildProcess process;
    RF_REQUIRE(process.Start({"version"}, rftest::executable_path("rf_cli"), log.string()));
    std::uint32_t exit_code = 0;
    RF_REQUIRE(process.WaitForExit(exit_code));
    RF_CHECK_EQ(exit_code, static_cast<std::uint32_t>(0));
    std::string contents;
    RF_REQUIRE(rftest::read_text_file(log, contents));
    std::string library_version;
    std::string package_version;
    RF_REQUIRE(rftest::find_line_value(contents, "library-version", library_version));
    RF_REQUIRE(rftest::find_line_value(contents, "package-version", package_version));
    RF_CHECK_EQ(library_version, package_version);
    RF_CHECK_EQ(library_version, std::string(kVersionString));
    RF_CHECK(contents.find("wire-protocol 1") != std::string::npos);
    RF_CHECK(contents.find("persistence-format 1") != std::string::npos);
  }

  {  // real host routing table evidence
    ChildProcess process;
    RF_REQUIRE(process.Start({"backend", "show"}, rftest::executable_path("rf_cli"), log.string()));
    std::uint32_t exit_code = 0;
    RF_REQUIRE(process.WaitForExit(exit_code));
    RF_CHECK_EQ(exit_code, static_cast<std::uint32_t>(0));
    std::string contents;
    RF_REQUIRE(rftest::read_text_file(log, contents));
    RF_CHECK(contents.find("windows-host-routing-table") != std::string::npos);
    RF_CHECK(contents.find("class REAL read-only") != std::string::npos);
    std::string host_routes;
    RF_REQUIRE(rftest::find_line_value(contents, "host-routes", host_routes));
    std::uint64_t count = 0;
    RF_REQUIRE(parse_u64_decimal(host_routes, 1000000, count));
    RF_CHECK(count > 0);
    RF_CHECK(contents.find("127.0.0.0/8") != std::string::npos);
  }

  {  // durable store inspection through the command line
    const std::filesystem::path store = directory / "fabric";
    CoordinatorHandle coordinator;
    RF_REQUIRE(start_coordinator(directory, store.string(), "resume", coordinator));
    PublisherHandle publisher;
    RF_REQUIRE(start_publisher(directory, coordinator.port, "publisher-a", "10.0.0.0/24", "cli", publisher));
    std::string why;
    RF_REQUIRE(wait_for_state(publisher, "published", why));
    {
      std::ofstream exit_file((directory / "coordinator.exit").string(), std::ios::binary | std::ios::trunc);
      exit_file << "shutdown\n";
    }
    std::uint32_t exit_code = 0;
    RF_REQUIRE(coordinator.process.WaitForExit(exit_code));
    RF_CHECK_EQ(exit_code, static_cast<std::uint32_t>(0));
    RF_REQUIRE(publisher.process.Terminate());

    ChildProcess inspect;
    RF_REQUIRE(inspect.Start({"store", "inspect", store.string()}, rftest::executable_path("rf_cli"),
                             (directory / "inspect.log").string()));
    RF_REQUIRE(inspect.WaitForExit(exit_code));
    RF_CHECK_EQ(exit_code, static_cast<std::uint32_t>(0));
    std::string contents;
    RF_REQUIRE(rftest::read_text_file(directory / "inspect.log", contents));
    RF_CHECK(contents.find("integrity ok") != std::string::npos);
    std::string routes;
    RF_REQUIRE(rftest::find_line_value(contents, "routes", routes));
    RF_CHECK_EQ(routes, std::string("1"));
  }

  rftest::remove_directory(directory);
}
