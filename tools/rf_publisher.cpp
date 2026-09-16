// Route Fabric publisher worker process.
//
// Registers a publisher with a coordinator, publishes one route, and then either
// exits or holds the session open. The status file is the deterministic
// synchronization channel used by the process-level tests.

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "routefabric/client.hpp"
#include "routefabric/route_key.hpp"
#include "routefabric/version.hpp"
#include "tools_common.hpp"

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string publisher = "publisher-a";
  std::string boot;
  std::string fabric = "fabric";
  std::string routing_namespace = "default";
  std::string destination = "ipv4:10.0.0.0/24";
  std::string route_class = "unicast";
  std::string next_hop;
  std::string next_hop_group;
  std::string path;
  std::string path_generation;
  std::string status_file;
  std::string seed;
  std::string reason = "worker publication";
  bool hold = false;
  bool withdraw = false;
  bool administrative = false;
};

void usage() {
  routefabric::tools::print_line("usage: rf_publisher [options]");
  routefabric::tools::print_line("  --port <n>                coordinator TCP port (required)");
  routefabric::tools::print_line("  --host <address>          coordinator address (default 127.0.0.1)");
  routefabric::tools::print_line("  --publisher <id>          publisher identity");
  routefabric::tools::print_line("  --boot <hex>              explicit worker boot identity");
  routefabric::tools::print_line("  --fabric <id>             authority domain");
  routefabric::tools::print_line("  --namespace <id>          routing namespace (default default)");
  routefabric::tools::print_line("  --destination <dest>      destination, for example 10.0.0.0/24");
  routefabric::tools::print_line("  --class <class>           unicast | host | service | overlay | logical");
  routefabric::tools::print_line("  --next-hop <hex>          next-hop identity (16 bytes, hex)");
  routefabric::tools::print_line("  --next-hop-group <hex>    next-hop group identity");
  routefabric::tools::print_line("  --path <hex>              authorized path identity");
  routefabric::tools::print_line("  --path-generation <n>     path authority generation for --path");
  routefabric::tools::print_line("  --status-file <path>      deterministic status channel");
  routefabric::tools::print_line("  --seed <n>                deterministic identity seed");
  routefabric::tools::print_line("  --hold                    keep the session open after publishing");
  routefabric::tools::print_line("  --withdraw                withdraw the published route");
  routefabric::tools::print_line("  --administrative          register with an administrative override scope");
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") {
      usage();
      return false;
    }
    if (argument == "--hold") {
      options.hold = true;
      continue;
    }
    if (argument == "--withdraw") {
      options.withdraw = true;
      continue;
    }
    if (argument == "--administrative") {
      options.administrative = true;
      continue;
    }
    if (i + 1 >= argc) {
      routefabric::tools::print_line("error: missing value for " + argument);
      return false;
    }
    const std::string value = argv[++i];
    if (argument == "--host") {
      options.host = value;
    } else if (argument == "--port") {
      std::uint64_t parsed = 0;
      if (!routefabric::parse_u64_decimal(value, 65535, parsed)) {
        routefabric::tools::print_line("error: --port must be a decimal port number");
        return false;
      }
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--publisher") {
      options.publisher = value;
    } else if (argument == "--boot") {
      options.boot = value;
    } else if (argument == "--fabric") {
      options.fabric = value;
    } else if (argument == "--namespace") {
      options.routing_namespace = value;
    } else if (argument == "--destination") {
      options.destination = value;
    } else if (argument == "--class") {
      options.route_class = value;
    } else if (argument == "--next-hop") {
      options.next_hop = value;
    } else if (argument == "--next-hop-group") {
      options.next_hop_group = value;
    } else if (argument == "--path") {
      options.path = value;
    } else if (argument == "--path-generation") {
      options.path_generation = value;
    } else if (argument == "--status-file") {
      options.status_file = value;
    } else if (argument == "--seed") {
      options.seed = value;
    } else {
      routefabric::tools::print_line("error: unknown option " + argument);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace routefabric;
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }
  if (options.port == 0) {
    tools::print_line("error: --port is required");
    return 2;
  }

  const auto publish_status = [&options](const std::string& state, const std::string& extra) {
    std::string status = "state=" + state + "\n";
    status += extra;
    if (!options.status_file.empty()) {
      tools::write_status_file(options.status_file, status);
    }
  };

  const Expected<PublisherId> publisher = PublisherId::parse(options.publisher);
  if (!publisher) {
    publish_status("failed", "error=" + publisher.error().detail() + "\n");
    tools::print_error(publisher.error());
    return 2;
  }
  const Expected<FabricId> fabric = FabricId::parse(options.fabric);
  if (!fabric) {
    publish_status("failed", "error=" + fabric.error().detail() + "\n");
    tools::print_error(fabric.error());
    return 2;
  }
  const Expected<RoutingNamespace> routing_namespace = RoutingNamespace::parse(options.routing_namespace);
  if (!routing_namespace) {
    publish_status("failed", "error=" + routing_namespace.error().detail() + "\n");
    tools::print_error(routing_namespace.error());
    return 2;
  }
  const Expected<Destination> destination = Destination::parse(options.destination, 128);
  if (!destination) {
    publish_status("failed", "error=" + destination.error().detail() + "\n");
    tools::print_error(destination.error());
    return 2;
  }
  RouteClass route_class = RouteClass::Unicast;
  if (!parse_route_class(options.route_class, route_class)) {
    publish_status("failed", "error=unknown route class\n");
    tools::print_line("error: unknown route class");
    return 2;
  }

  ClientConfig client_config;
  client_config.host = options.host;
  client_config.port = options.port;
  if (!options.seed.empty()) {
    std::uint64_t seed = 0;
    if (!parse_u64_decimal(options.seed, 0xFFFFFFFFFFFFFFFFull, seed)) {
      tools::print_line("error: --seed must be a decimal integer");
      return 2;
    }
    client_config.id_seed = seed;
  }
  RouteFabricClient client(client_config);
  const Status connected = client.Connect();
  if (!connected) {
    publish_status("failed", "error=" + connected.error().detail() + "\n");
    tools::print_error(connected.error());
    return 1;
  }

  const Expected<HelloResult> hello = client.Hello("rf_publisher");
  if (!hello || hello.value().status != StatusCode::Ok) {
    publish_status("failed", "error=hello failed\n");
    tools::print_line("error: hello failed");
    return 1;
  }

  // Every publisher process incarnation gets a fresh worker boot identity unless
  // one is supplied explicitly.
  if (options.boot.empty()) {
    // A worker boot identity must be unique to this process incarnation, so it is
    // derived from platform entropy even when the mutation-attempt seed is fixed.
    const std::uint64_t seed = mix64((client_config.id_seed != 0 ? client_config.id_seed
                                                                 : SeededIdSource::system_seed()) ^
                                     SeededIdSource::system_seed());
    SeededIdSource source(seed);
    client.context().worker_boot = generate_id<WorkerBootId>(source);
  } else {
    const Expected<WorkerBootId> parsed = WorkerBootId::parse(options.boot);
    if (!parsed) {
      publish_status("failed", "error=" + parsed.error().detail() + "\n");
      tools::print_error(parsed.error());
      return 2;
    }
    client.context().worker_boot = parsed.value();
  }
  client.context().publisher = publisher.value();

  PublisherScope scope;
  scope.fabric = fabric.value();
  scope.wildcard_namespaces = true;
  scope.wildcard_destinations = true;
  scope.wildcard_route_classes = true;
  scope.administrative_override = options.administrative;

  client.next_attempt();
  const Expected<RegisterPublisherResult> registration = client.RegisterPublisher(scope);
  if (!registration || registration.value().status != StatusCode::Ok) {
    const std::string detail = registration ? registration.value().detail : registration.error().detail();
    publish_status("failed", "error=" + detail + "\n");
    tools::print_line("error: registration failed: " + detail);
    return 1;
  }
  const std::string boot_text = client.context().worker_boot.render();
  publish_status("registered",
                 "boot=" + boot_text + "\npublisher=" + publisher.value().render() + "\nepoch=" +
                     registration.value().epoch.render() + "\nport=" + to_decimal(options.port) + "\n");
  tools::print_line("registered publisher=" + publisher.value().render() + " boot=" + boot_text);
  tools::flush_output();

  RouteKey key;
  key.fabric = fabric.value();
  key.routing_namespace = routing_namespace.value();
  key.destination = destination.value();
  key.route_class = route_class;

  RouteBinding binding;
  if (!options.path.empty()) {
    const Expected<PathId> path = PathId::parse(options.path);
    const Expected<PathAuthorityGeneration> generation = PathAuthorityGeneration::parse(options.path_generation);
    if (!path || !generation) {
      publish_status("failed", "error=malformed path binding\n");
      tools::print_line("error: malformed path binding");
      return 2;
    }
    binding.kind = BindingKind::AuthorizedPath;
    binding.path = path.value();
    binding.path_authority_generation = generation.value();
    binding.policy_generation = PolicyGeneration::from_value(1);
    binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
  } else {
    binding.kind = BindingKind::NextHop;
    binding.policy_generation = PolicyGeneration::from_value(1);
    binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
    if (!options.next_hop_group.empty()) {
      const Expected<NextHopGroupId> group = NextHopGroupId::parse(options.next_hop_group);
      if (!group) {
        publish_status("failed", "error=malformed next-hop group\n");
        tools::print_line("error: malformed next-hop group identity");
        return 2;
      }
      binding.next_hop.kind = NextHopKind::NextHopGroup;
      binding.next_hop.next_hop_group = group.value();
    } else if (!options.next_hop.empty()) {
      const Expected<NextHopId> next_hop = NextHopId::parse(options.next_hop);
      if (!next_hop) {
        publish_status("failed", "error=malformed next hop\n");
        tools::print_line("error: malformed next-hop identity");
        return 2;
      }
      binding.next_hop.kind = NextHopKind::DirectEndpoint;
      binding.next_hop.next_hop = next_hop.value();
    } else {
      binding.next_hop.kind = NextHopKind::DirectEndpoint;
      binding.next_hop.next_hop = NextHopId::from_digest(
          digest128("routefabric.publisher.next-hop", std::string("default")));
    }
  }

  client.next_attempt();
  const Expected<PublishRouteResult> published = client.PublishRoute(
      key, binding, PolicyGeneration::from_value(1), RouteGeneration(), RouteId(), options.reason);
  if (!published || published.value().status != StatusCode::Ok) {
    const std::string detail = published ? published.value().detail : published.error().detail();
    publish_status("failed", "error=" + detail + "\n");
    tools::print_line("error: publication failed: " + detail);
    return 1;
  }
  const std::string route_text = published.value().route.render();
  publish_status("published",
                 "boot=" + boot_text + "\npublisher=" + publisher.value().render() +
                     "\nepoch=" + registration.value().epoch.render() + "\nport=" + to_decimal(options.port) +
                     "\nroute-id=" + route_text + "\nroute-generation=" + published.value().generation.render() +
                     "\nlifecycle=" + to_string(published.value().lifecycle) + "\napplied=" +
                     to_string(published.value().applied) + "\ncurrentness=" +
                     to_string(published.value().currentness) + "\n");
  tools::print_line("published route=" + route_text + " lifecycle=" + to_string(published.value().lifecycle) +
                    " applied=" + to_string(published.value().applied));
  tools::flush_output();

  if (options.withdraw) {
    client.next_attempt();
    const Expected<RouteMutationResult> withdrawn = client.WithdrawRoute(published.value().route, RouteGeneration(), "worker withdrawal");
    if (!withdrawn || withdrawn.value().status != StatusCode::Ok) {
      const std::string detail = withdrawn ? withdrawn.value().detail : withdrawn.error().detail();
      publish_status("failed", "error=" + detail + "\n");
      tools::print_line("error: withdrawal failed: " + detail);
      return 1;
    }
    publish_status("withdrawn",
                   "boot=" + boot_text + "\nroute-id=" + route_text + "\nlifecycle=" +
                       to_string(withdrawn.value().lifecycle) + "\napplied=" +
                       to_string(withdrawn.value().applied) + "\n");
    tools::print_line("withdrawn route=" + route_text + " lifecycle=" + to_string(withdrawn.value().lifecycle));
    tools::flush_output();
  }

  if (options.hold) {
    // Hold the session open until the process is terminated.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  client.Close();
  return 0;
}
