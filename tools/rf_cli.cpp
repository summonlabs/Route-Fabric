// Route Fabric operator CLI.
//
// Deterministic, script-friendly output. Direct mode operates on a local runtime
// and store; wire mode talks to a running coordinator.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/client.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/persistence.hpp"
#include "routefabric/real_backend.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/snapshot.hpp"
#include "routefabric/version.hpp"
#include "tools_common.hpp"

namespace {

using namespace routefabric;

struct Options {
  std::string store;
  std::string durability = "none";
  std::string fabric = "fabric";
  std::string publisher = "cli-admin";
  std::string routing_namespace = "default";
  std::string route_class = "unicast";
  std::string host = "127.0.0.1";
  std::string paths_file;
  std::string seed;
  std::string path;
  std::string path_generation;
  std::string next_hop;
  std::string expected_generation;
  std::string reason = "operator action";
  std::uint16_t port = 0;
  bool administrative = false;
};

struct PathEntry {
  PathId path;
  PathAuthorityGeneration generation;
  PathAuthorization state = PathAuthorization::Usable;
};

void usage() {
  tools::print_line("usage: rf_cli [options] <group> <command> [arguments]");
  tools::print_line("");
  tools::print_line("options:");
  tools::print_line("  --store <path>            durable store base path");
  tools::print_line("  --durability <mode>       none | snapshot | journal (default none)");
  tools::print_line("  --port <n>                coordinator port; enables wire mode");
  tools::print_line("  --host <address>          coordinator address (default 127.0.0.1)");
  tools::print_line("  --fabric <id>             authority domain (default fabric)");
  tools::print_line("  --publisher <id>          acting publisher identity (default cli-admin)");
  tools::print_line("  --namespace <id>          routing namespace (default default)");
  tools::print_line("  --class <class>           route class (default unicast)");
  tools::print_line("  --next-hop <hex>          next-hop identity (16 bytes hex)");
  tools::print_line("  --path <hex>              authorized path identity");
  tools::print_line("  --path-generation <n>     path authority generation");
  tools::print_line("  --expected-generation <n> optimistic concurrency check");
  tools::print_line("  --paths <file>            path authority bootstrap file");
  tools::print_line("  --administrative          register with an administrative override scope");
  tools::print_line("  --seed <n>                deterministic identity seed");
  tools::print_line("  --reason <text>           reason recorded with the mutation");
  tools::print_line("");
  tools::print_line("commands:");
  tools::print_line("  version");
  tools::print_line("  route publish <destination>");
  tools::print_line("  route list | show <route-id|destination> | explain <route-id|destination>");
  tools::print_line("  route withdraw|revalidate|retire|revoke <route-id>");
  tools::print_line("  route reconcile [<route-id>]");
  tools::print_line("  route snapshot");
  tools::print_line("  store inspect | store diff <path-a> <path-b> [<route-id>]");
  tools::print_line("  backend show");
  tools::print_line("  path add <path-id> <generation> <state> | path invalidate <path-id> <state> | path list");
}

bool parse_options(std::vector<std::string>& arguments, Options& options) {
  std::vector<std::string> remaining;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    }
    if (argument == "--administrative") {
      options.administrative = true;
      continue;
    }
    if (argument == "--version") {
      remaining.push_back("version");
      continue;
    }
    if (argument.rfind("--", 0) != 0) {
      remaining.push_back(argument);
      continue;
    }
    if (i + 1 >= arguments.size()) {
      tools::print_line("error: missing value for " + argument);
      return false;
    }
    const std::string value = arguments[++i];
    if (argument == "--store") {
      options.store = value;
    } else if (argument == "--durability") {
      options.durability = value;
    } else if (argument == "--fabric") {
      options.fabric = value;
    } else if (argument == "--publisher") {
      options.publisher = value;
    } else if (argument == "--namespace") {
      options.routing_namespace = value;
    } else if (argument == "--class") {
      options.route_class = value;
    } else if (argument == "--host") {
      options.host = value;
    } else if (argument == "--path") {
      options.path = value;
    } else if (argument == "--path-generation") {
      options.path_generation = value;
    } else if (argument == "--next-hop") {
      options.next_hop = value;
    } else if (argument == "--expected-generation") {
      options.expected_generation = value;
    } else if (argument == "--paths") {
      options.paths_file = value;
    } else if (argument == "--seed") {
      options.seed = value;
    } else if (argument == "--reason") {
      options.reason = value;
    } else if (argument == "--port") {
      std::uint64_t parsed = 0;
      if (!parse_u64_decimal(value, 65535, parsed)) {
        tools::print_line("error: --port must be a decimal port number");
        return false;
      }
      options.port = static_cast<std::uint16_t>(parsed);
    } else {
      tools::print_line("error: unknown option " + argument);
      return false;
    }
  }
  arguments = remaining;
  return true;
}

std::vector<PathEntry> load_paths(const std::string& file, bool& ok) {
  std::vector<PathEntry> entries;
  ok = true;
  if (file.empty()) {
    return entries;
  }
  std::ifstream input(file);
  if (!input) {
    ok = false;
    return entries;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream stream(line);
    std::string path_text;
    std::string generation_text;
    std::string state_text;
    std::string extra;
    if (!(stream >> path_text >> generation_text >> state_text) || (stream >> extra)) {
      ok = false;
      return entries;
    }
    const Expected<PathId> path = PathId::parse(path_text);
    const Expected<PathAuthorityGeneration> generation = PathAuthorityGeneration::parse(generation_text);
    PathAuthorization state = PathAuthorization::Usable;
    if (!path || !generation || !parse_path_authorization(state_text, state)) {
      ok = false;
      return entries;
    }
    entries.push_back(PathEntry{path.value(), generation.value(), state});
  }
  return entries;
}

bool save_paths(const std::string& file, const std::vector<PathEntry>& entries) {
  std::ostringstream output;
  output << "# path-id path-authority-generation state\n";
  for (const PathEntry& entry : entries) {
    output << entry.path.render() << " " << entry.generation.render() << " " << to_string(entry.state) << "\n";
  }
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << output.str();
  out.flush();
  return static_cast<bool>(out);
}

PathEntry* find_path(std::vector<PathEntry>& entries, const PathId& path) {
  for (PathEntry& entry : entries) {
    if (entry.path == path) {
      return &entry;
    }
  }
  return nullptr;
}

struct DirectRuntime {
  std::unique_ptr<SyntheticProgrammingBackend> backend;
  std::unique_ptr<SyntheticPathAuthority> path_authority;
  std::unique_ptr<RouteFabricRuntime> runtime;
};

bool build_direct_runtime(const Options& options, DirectRuntime& direct, const std::vector<PathEntry>& paths,
                          PublisherRegistration& registration, std::string& failure) {
  const Expected<BackendId> backend_id = BackendId::parse("synthetic-programming");
  direct.backend = std::make_unique<SyntheticProgrammingBackend>(backend_id.value());
  direct.path_authority = std::make_unique<SyntheticPathAuthority>();
  for (const PathEntry& entry : paths) {
    const Status status = direct.path_authority->SetPath(entry.path, entry.generation, entry.state);
    if (!status) {
      failure = status.error().detail();
      return false;
    }
  }
  const Expected<FabricId> fabric = FabricId::parse(options.fabric);
  if (!fabric) {
    failure = fabric.error().detail();
    return false;
  }
  RuntimeConfig config;
  config.fabric = fabric.value();
  if (!parse_durability(options.durability, config.durability)) {
    failure = "--durability must be none, snapshot or journal";
    return false;
  }
  config.store_path = options.store;
  if (!options.seed.empty()) {
    std::uint64_t seed = 0;
    if (!parse_u64_decimal(options.seed, 0xFFFFFFFFFFFFFFFFull, seed)) {
      failure = "--seed must be a decimal integer";
      return false;
    }
    config.id_seed = seed;
  }
  direct.runtime = std::make_unique<RouteFabricRuntime>(config, direct.backend.get(), direct.path_authority.get());
  const Status opened = direct.runtime->Open();
  if (!opened) {
    failure = opened.error().detail();
    return false;
  }
  const Expected<PublisherId> publisher = PublisherId::parse(options.publisher);
  if (!publisher) {
    failure = publisher.error().detail();
    return false;
  }
  const Expected<RoutingNamespace> routing_namespace = RoutingNamespace::parse(options.routing_namespace);
  if (!routing_namespace) {
    failure = routing_namespace.error().detail();
    return false;
  }
  RouteClass route_class = RouteClass::Unicast;
  if (!parse_route_class(options.route_class, route_class)) {
    failure = "unknown route class";
    return false;
  }
  PublisherScope scope;
  scope.fabric = fabric.value();
  scope.wildcard_namespaces = true;
  scope.wildcard_destinations = true;
  scope.wildcard_route_classes = true;
  scope.administrative_override = options.administrative;
  const Expected<PublisherRegistration> registered =
      direct.runtime->RegisterPublisher(publisher.value(), WorkerBootId::from_digest(digest128("routefabric.cli.boot", options.publisher)), scope);
  if (!registered) {
    failure = registered.error().detail();
    return false;
  }
  registration = registered.value();
  return true;
}

Expected<RouteKey> build_key(const Options& options, const std::string& destination_text) {
  const Expected<FabricId> fabric = FabricId::parse(options.fabric);
  const Expected<RoutingNamespace> routing_namespace = RoutingNamespace::parse(options.routing_namespace);
  const Expected<Destination> destination = Destination::parse(destination_text, 128);
  RouteClass route_class = RouteClass::Unicast;
  if (!fabric) {
    return make_error<RouteKey>(fabric.error().code(), fabric.error().detail());
  }
  if (!routing_namespace) {
    return make_error<RouteKey>(routing_namespace.error().code(), routing_namespace.error().detail());
  }
  if (!destination) {
    return make_error<RouteKey>(destination.error().code(), destination.error().detail());
  }
  if (!parse_route_class(options.route_class, route_class)) {
    return make_error<RouteKey>(StatusCode::InvalidArgument, "unknown route class");
  }
  RouteKey key;
  key.fabric = fabric.value();
  key.routing_namespace = routing_namespace.value();
  key.destination = destination.value();
  key.route_class = route_class;
  if (!key.is_valid()) {
    return make_error<RouteKey>(StatusCode::InvalidArgument, "the route key is not valid for that route class");
  }
  return key;
}

RouteBinding build_binding(const Options& options, const std::vector<PathEntry>& paths, std::string& failure) {
  RouteBinding binding;
  binding.policy_generation = PolicyGeneration::from_value(1);
  binding.next_hop.entity_generation = PolicyGeneration::from_value(1);
  if (!options.path.empty()) {
    const Expected<PathId> path = PathId::parse(options.path);
    if (!path) {
      failure = path.error().detail();
      return binding;
    }
    PathAuthorityGeneration generation;
    if (!options.path_generation.empty()) {
      const Expected<PathAuthorityGeneration> parsed = PathAuthorityGeneration::parse(options.path_generation);
      if (!parsed) {
        failure = parsed.error().detail();
        return binding;
      }
      generation = parsed.value();
    } else {
      bool found = false;
      for (const PathEntry& entry : paths) {
        if (entry.path == path.value()) {
          generation = entry.generation;
          found = true;
          break;
        }
      }
      if (!found) {
        failure = "the path is unknown to the path authority file; supply --path-generation";
        return binding;
      }
    }
    binding.kind = BindingKind::AuthorizedPath;
    binding.path = path.value();
    binding.path_authority_generation = generation;
    return binding;
  }
  binding.kind = BindingKind::NextHop;
  binding.next_hop.kind = NextHopKind::DirectEndpoint;
  if (!options.next_hop.empty()) {
    const Expected<NextHopId> next_hop = NextHopId::parse(options.next_hop);
    if (!next_hop) {
      failure = next_hop.error().detail();
      return binding;
    }
    binding.next_hop.next_hop = next_hop.value();
  } else {
    binding.next_hop.next_hop = NextHopId::from_digest(digest128("routefabric.cli.next-hop", std::string("default")));
  }
  return binding;
}

Expected<RouteGeneration> parse_expected_generation(const Options& options) {
  if (options.expected_generation.empty()) {
    return RouteGeneration();
  }
  return RouteGeneration::parse(options.expected_generation);
}

std::string render_persisted_record(const RouteRecord& record) {
  std::string out = record.id.render();
  out += " key=";
  out += record.key.render();
  out += " generation=";
  out += record.generation.render();
  out += " authority-generation=";
  out += record.authority_generation.render();
  out += " lifecycle=";
  out += to_string(record.lifecycle);
  out += " applied=";
  out += to_string(record.applied.classification);
  out += " binding=";
  out += record.binding.render();
  out += " publisher=";
  out += record.provenance.publisher.render();
  out += " epoch=";
  out += record.provenance.epoch.render();
  return out;
}

int store_diff(const std::string& path_a, const std::string& path_b, const std::string& route_filter) {
  Limits limits;
  RouteStore store_a(path_a, limits, Durability::Snapshot);
  RouteStore store_b(path_b, limits, Durability::Snapshot);
  std::string why;
  Expected<PersistedState> state_a = store_a.Load();
  if (!state_a) {
    tools::print_error(state_a.error());
    return 1;
  }
  Expected<PersistedState> state_b = store_b.Load();
  if (!state_b) {
    tools::print_error(state_b.error());
    return 1;
  }
  std::map<RouteId, const RouteRecord*> a;
  std::map<RouteId, const RouteRecord*> b;
  for (const RouteRecord& record : state_a.value().routes) {
    a[record.id] = &record;
  }
  for (const RouteRecord& record : state_b.value().routes) {
    b[record.id] = &record;
  }
  std::set<RouteId> ids;
  for (const auto& pair : a) {
    ids.insert(pair.first);
  }
  for (const auto& pair : b) {
    ids.insert(pair.first);
  }
  int differences = 0;
  tools::print_line("epoch-a " + state_a.value().epoch.render() + " epoch-b " + state_b.value().epoch.render());
  for (const RouteId& id : ids) {
    if (!route_filter.empty()) {
      const Expected<RouteId> wanted = RouteId::parse(route_filter);
      if (!wanted || !(wanted.value() == id)) {
        continue;
      }
    }
    const RouteRecord* left = a.count(id) != 0 ? a[id] : nullptr;
    const RouteRecord* right = b.count(id) != 0 ? b[id] : nullptr;
    if (left == nullptr || right == nullptr) {
      ++differences;
      tools::print_line(std::string("route ") + id.render() + " presence " +
                        (left == nullptr ? "absent" : "present") + " -> " +
                        (right == nullptr ? "absent" : "present"));
      continue;
    }
    if (semantic_digest(*left) == semantic_digest(*right)) {
      continue;
    }
    ++differences;
    tools::print_line(std::string("route ") + id.render());
    if (left->generation != right->generation) {
      tools::print_line("  generation " + left->generation.render() + " -> " + right->generation.render());
    }
    if (left->authority_generation != right->authority_generation) {
      tools::print_line("  authority-generation " + left->authority_generation.render() + " -> " +
                        right->authority_generation.render());
    }
    if (left->lifecycle != right->lifecycle) {
      tools::print_line(std::string("  lifecycle ") + to_string(left->lifecycle) + " -> " +
                        to_string(right->lifecycle));
    }
    if (!(left->binding == right->binding)) {
      tools::print_line("  binding " + left->binding.render() + " -> " + right->binding.render());
    }
    if (left->applied.classification != right->applied.classification) {
      tools::print_line(std::string("  applied ") + to_string(left->applied.classification) + " -> " +
                        to_string(right->applied.classification));
    }
    if (!(left->provenance.epoch == right->provenance.epoch)) {
      tools::print_line("  epoch " + left->provenance.epoch.render() + " -> " + right->provenance.epoch.render());
    }
    if (!(left->provenance.publisher == right->provenance.publisher)) {
      tools::print_line("  publisher " + left->provenance.publisher.render() + " -> " +
                        right->provenance.publisher.render());
    }
  }
  tools::print_line("differences " + to_decimal(static_cast<std::uint64_t>(differences)));
  return 0;
}

int store_inspect(const std::string& path, const Limits& limits) {
  RouteStore store(path, limits, Durability::Snapshot);
  if (!store.exists()) {
    tools::print_line("error: no store at " + path);
    return 1;
  }
  Expected<PersistedState> state = store.Load();
  if (!state) {
    tools::print_error(state.error());
    return 1;
  }
  tools::print_line("store " + path);
  tools::print_line("format-version " + to_decimal(kPersistenceFormatVersion));
  tools::print_line("integrity ok");
  tools::print_line("epoch " + state.value().epoch.render());
  tools::print_line("policy-generation " + state.value().policy_generation.render());
  tools::print_line("routes " + to_decimal(state.value().routes.size()));
  tools::print_line("revocations " + to_decimal(state.value().revocations.size()));
  tools::print_line("programming-attempts " + to_decimal(state.value().attempts.size()));
  tools::print_line("digest " + state.value().digest().to_hex());
  for (const RouteRecord& record : state.value().routes) {
    tools::print_line("  " + render_persisted_record(record));
  }
  return 0;
}

std::string render_counters(const RouteCounters& counters) {
  std::string out = "publications " + to_decimal(counters.publications) + "\n";
  out += "replacements " + to_decimal(counters.replacements) + "\n";
  out += "idempotent-publications " + to_decimal(counters.idempotent_publications) + "\n";
  out += "withdrawals " + to_decimal(counters.withdrawals) + "\n";
  out += "revalidations " + to_decimal(counters.revalidations) + "\n";
  out += "retirements " + to_decimal(counters.retirements) + "\n";
  out += "revocations " + to_decimal(counters.revocations) + "\n";
  out += "supersessions " + to_decimal(counters.supersessions) + "\n";
  out += "programming-dispatches " + to_decimal(counters.programming_dispatches) + "\n";
  out += "programming-deferred " + to_decimal(counters.programming_deferred) + "\n";
  out += "programming-applied " + to_decimal(counters.programming_applied) + "\n";
  out += "programming-ambiguous " + to_decimal(counters.programming_ambiguous) + "\n";
  out += "stale-completions-rejected " + to_decimal(counters.stale_completions_rejected) + "\n";
  out += "stale-authority-rejections " + to_decimal(counters.stale_authority_rejections) + "\n";
  out += "scope-violations " + to_decimal(counters.scope_violations) + "\n";
  out += "path-authority-rejections " + to_decimal(counters.path_authority_rejections) + "\n";
  out += "conflicts " + to_decimal(counters.conflicts) + "\n";
  out += "reconciliations " + to_decimal(counters.reconciliations);
  return out;
}

std::string describe_backend(IRouteProgrammingBackend& backend) {
  const BackendCapabilities capabilities = backend.capabilities();
  std::string out = "backend " + backend.id().render() + "\n";
  out += std::string("class ") + (capabilities.read_only ? "REAL read-only" : "SYNTHETIC") + "\n";
  out += std::string("install ") + (capabilities.install ? "yes" : "no") + "\n";
  out += std::string("replace ") + (capabilities.replace ? "yes" : "no") + "\n";
  out += std::string("withdraw ") + (capabilities.withdraw ? "yes" : "no") + "\n";
  out += std::string("query ") + (capabilities.query ? "yes" : "no");
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.push_back(argv[i]);
  }
  Options options;
  if (!parse_options(arguments, options)) {
    return 2;
  }
  if (arguments.empty()) {
    usage();
    return 2;
  }
  const std::string group = arguments[0];
  const std::string command = arguments.size() > 1 ? arguments[1] : std::string();
  const std::vector<std::string> rest(arguments.begin() + std::min<std::size_t>(2, arguments.size()),
                                      arguments.end());

  Limits limits;
  if (group == "version") {
    tools::print_line(std::string("route-fabric ") + std::string(kVersionString));
    tools::print_line("library-version " + std::string(kVersionString));
    tools::print_line("package-version " + std::string(kVersionString));
    tools::print_line("wire-protocol " + to_decimal(kWireProtocolVersion));
    tools::print_line("persistence-format " + to_decimal(kPersistenceFormatVersion));
    return 0;
  }

  if (group == "store") {
    if (command == "inspect") {
      if (rest.empty()) {
        tools::print_line("error: store inspect requires a store path");
        return 2;
      }
      return store_inspect(rest[0], limits);
    }
    if (command == "diff") {
      if (rest.size() < 2) {
        tools::print_line("error: store diff requires two store paths");
        return 2;
      }
      return store_diff(rest[0], rest[1], rest.size() > 2 ? rest[2] : std::string());
    }
    usage();
    return 2;
  }

  if (group == "path") {
    if (options.paths_file.empty()) {
      tools::print_line("error: path commands require --paths <file>");
      return 2;
    }
    bool ok = true;
    std::vector<PathEntry> entries = load_paths(options.paths_file, ok);
    if (!ok) {
      tools::print_line("error: malformed path authority file");
      return 1;
    }
    if (command == "list") {
      tools::print_line("# path-id path-authority-generation state");
      for (const PathEntry& entry : entries) {
        tools::print_line(entry.path.render() + " " + entry.generation.render() + " " + to_string(entry.state));
      }
      return 0;
    }
    if (command == "add") {
      if (rest.size() < 3) {
        tools::print_line("error: path add requires <path-id> <generation> <state>");
        return 2;
      }
      const Expected<PathId> path = PathId::parse(rest[0]);
      const Expected<PathAuthorityGeneration> generation = PathAuthorityGeneration::parse(rest[1]);
      PathAuthorization state = PathAuthorization::Usable;
      if (!path || !generation || !parse_path_authorization(rest[2], state)) {
        tools::print_line("error: malformed path entry");
        return 2;
      }
      PathEntry* existing = find_path(entries, path.value());
      if (existing == nullptr) {
        entries.push_back(PathEntry{path.value(), generation.value(), state});
      } else {
        existing->generation = generation.value();
        existing->state = state;
      }
      if (!save_paths(options.paths_file, entries)) {
        tools::print_line("error: cannot write the path authority file");
        return 1;
      }
      tools::print_line("path " + path.value().render() + " generation " + generation.value().render() + " state " +
                        to_string(state));
      return 0;
    }
    if (command == "invalidate") {
      if (rest.size() < 2) {
        tools::print_line("error: path invalidate requires <path-id> <state>");
        return 2;
      }
      const Expected<PathId> path = PathId::parse(rest[0]);
      PathAuthorization state = PathAuthorization::RevalidationRequired;
      if (!path || !parse_path_authorization(rest[1], state)) {
        tools::print_line("error: malformed path entry");
        return 2;
      }
      PathEntry* existing = find_path(entries, path.value());
      if (existing == nullptr) {
        tools::print_line("error: unknown path");
        return 1;
      }
      const Expected<PathAuthorityGeneration> next = existing->generation.next();
      if (!next) {
        tools::print_line("error: path authority generation would overflow");
        return 1;
      }
      existing->generation = next.value();
      existing->state = state;
      if (!save_paths(options.paths_file, entries)) {
        tools::print_line("error: cannot write the path authority file");
        return 1;
      }
      tools::print_line("path " + existing->path.render() + " generation " + existing->generation.render() +
                        " state " + to_string(existing->state));
      return 0;
    }
    usage();
    return 2;
  }

  bool paths_ok = true;
  const std::vector<PathEntry> paths = load_paths(options.paths_file, paths_ok);
  if (!paths_ok) {
    tools::print_line("error: malformed path authority file");
    return 1;
  }

  if (group == "backend") {
    if (command != "show") {
      usage();
      return 2;
    }
    WindowsRouteTableBackend real;
    if (WindowsRouteTableBackend::is_supported()) {
      tools::print_line(describe_backend(real));
      const std::vector<HostRouteEntry> entries = real.EnumerateHostRoutes();
      tools::print_line("host-routes " + to_decimal(entries.size()));
      std::size_t shown = 0;
      for (const HostRouteEntry& entry : entries) {
        if (shown >= 32) {
          break;
        }
        tools::print_line("  " + entry.render());
        ++shown;
      }
      if (entries.empty() && !real.last_error().empty()) {
        tools::print_line("  host-route-enumeration-error " + real.last_error());
      }
      return 0;
    }
    SyntheticProgrammingBackend synthetic(BackendId::parse("synthetic-programming").value());
    tools::print_line(describe_backend(synthetic));
    tools::print_line("host-routes 0");
    tools::print_line("  host-route-enumeration-error the host routing table adapter is unavailable on this platform");
    return 0;
  }

  if (group != "route") {
    usage();
    return 2;
  }

  // Wire mode: talk to a running coordinator.
  if (options.port != 0) {
    ClientConfig client_config;
    client_config.host = options.host;
    client_config.port = options.port;
    RouteFabricClient client(client_config);
    const Status connected = client.Connect();
    if (!connected) {
      tools::print_error(connected.error());
      return 1;
    }
    const Expected<HelloResult> hello = client.Hello("rf_cli");
    if (!hello || hello.value().status != StatusCode::Ok) {
      tools::print_line("error: hello failed");
      return 1;
    }
    const Expected<PublisherId> publisher = PublisherId::parse(options.publisher);
    if (!publisher) {
      tools::print_error(publisher.error());
      return 2;
    }
    client.context().publisher = publisher.value();
    const Expected<FabricId> fabric = FabricId::parse(options.fabric);
    if (!fabric) {
      tools::print_error(fabric.error());
      return 2;
    }
    PublisherScope scope;
    scope.fabric = fabric.value();
    scope.wildcard_namespaces = true;
    scope.wildcard_destinations = true;
    scope.wildcard_route_classes = true;
    scope.administrative_override = options.administrative;
    if (command == "publish" || command == "withdraw" || command == "revalidate" || command == "retire" ||
        command == "revoke" || command == "reconcile") {
      client.next_attempt();
      const Expected<RegisterPublisherResult> registration = client.RegisterPublisher(scope);
      if (!registration || registration.value().status != StatusCode::Ok) {
        tools::print_line("error: registration failed");
        return 1;
      }
    }
    if (command == "list" || command == "snapshot") {
      const Expected<SnapshotResponse> snapshot = client.Snapshot();
      if (!snapshot) {
        tools::print_error(snapshot.error());
        return 1;
      }
      tools::print_line("epoch " + snapshot.value().epoch.render());
      tools::print_line("route-count " + to_decimal(snapshot.value().routes.size()));
      tools::print_line("snapshot-id " + snapshot.value().snapshot_id.render());
      tools::print_line("digest " + snapshot.value().digest.to_hex());
      for (const RouteSnapshot& route : snapshot.value().routes) {
        tools::print_line("  " + route.render_compact());
      }
      return 0;
    }
    if (command == "show" || command == "explain") {
      if (rest.empty()) {
        tools::print_line("error: route " + command + " requires a route identity or destination");
        return 2;
      }
      Expected<RouteSnapshot> snapshot = make_error<RouteSnapshot>(StatusCode::NotFound, "no route");
      const Expected<RouteId> route_id = RouteId::parse(rest[0]);
      if (route_id) {
        snapshot = client.QueryRouteById(route_id.value());
      } else {
        const Expected<RouteKey> key = build_key(options, rest[0]);
        if (!key) {
          tools::print_error(key.error());
          return 2;
        }
        snapshot = client.QueryRouteByKey(key.value());
      }
      if (!snapshot) {
        tools::print_error(snapshot.error());
        return 1;
      }
      if (command == "show") {
        tools::print_line(snapshot.value().render());
        return 0;
      }
      const Expected<RouteId> explain_id = RouteId::parse(rest[0]);
      const Expected<RouteExplanation> explanation =
          explain_id ? client.ExplainRouteById(explain_id.value())
                     : client.ExplainRouteByKey(build_key(options, rest[0]).value());
      if (!explanation) {
        tools::print_error(explanation.error());
        return 1;
      }
      tools::print_line(explanation.value().render());
      return 0;
    }
    if (command == "publish") {
      if (rest.empty()) {
        tools::print_line("error: route publish requires a destination");
        return 2;
      }
      const Expected<RouteKey> key = build_key(options, rest[0]);
      if (!key) {
        tools::print_error(key.error());
        return 2;
      }
      std::string failure;
      const RouteBinding binding = build_binding(options, paths, failure);
      if (!failure.empty()) {
        tools::print_line("error: " + failure);
        return 2;
      }
      const Expected<RouteGeneration> expected = parse_expected_generation(options);
      if (!expected) {
        tools::print_error(expected.error());
        return 2;
      }
      client.next_attempt();
      const Expected<PublishRouteResult> published =
          client.PublishRoute(key.value(), binding, PolicyGeneration::from_value(1), expected.value(), RouteId(),
                              options.reason);
      if (!published) {
        tools::print_error(published.error());
        return 1;
      }
      if (published.value().status != StatusCode::Ok) {
        tools::print_line(std::string("error ") + to_string(published.value().status) + ": " + published.value().detail);
        return 1;
      }
      tools::print_line("route-id " + published.value().route.render());
      tools::print_line("generation " + published.value().generation.render());
      tools::print_line("lifecycle " + std::string(to_string(published.value().lifecycle)));
      tools::print_line("applied " + std::string(to_string(published.value().applied)));
      tools::print_line("currentness " + std::string(to_string(published.value().currentness)));
      return 0;
    }
    if (command == "withdraw" || command == "revalidate" || command == "retire" || command == "revoke") {
      if (rest.empty()) {
        tools::print_line("error: route " + command + " requires a route identity");
        return 2;
      }
      const Expected<RouteId> route_id = RouteId::parse(rest[0]);
      if (!route_id) {
        tools::print_error(route_id.error());
        return 2;
      }
      const Expected<RouteGeneration> expected = parse_expected_generation(options);
      if (!expected) {
        tools::print_error(expected.error());
        return 2;
      }
      client.next_attempt();
      Expected<RouteMutationResult> result = make_error<RouteMutationResult>(StatusCode::Internal, "unhandled");
      if (command == "withdraw") {
        result = client.WithdrawRoute(route_id.value(), expected.value(), options.reason);
      } else if (command == "revalidate") {
        result = client.RevalidateRoute(route_id.value(), options.reason);
      } else if (command == "retire") {
        result = client.RetireRoute(route_id.value(), options.reason);
      } else {
        result = client.RevokeRoute(route_id.value(), options.reason);
      }
      if (!result) {
        tools::print_error(result.error());
        return 1;
      }
      if (result.value().status != StatusCode::Ok) {
        tools::print_line(std::string("error ") + to_string(result.value().status) + ": " + result.value().detail);
        return 1;
      }
      tools::print_line("route-id " + result.value().route.render());
      tools::print_line("generation " + result.value().generation.render());
      tools::print_line("lifecycle " + std::string(to_string(result.value().lifecycle)));
      tools::print_line("applied " + std::string(to_string(result.value().applied)));
      return 0;
    }
    if (command == "reconcile") {
      const Expected<ReconcileResult> result =
          rest.empty() ? client.ReconcileAll() : client.ReconcileRoute(RouteId::parse(rest[0]).value());
      if (!result) {
        tools::print_error(result.error());
        return 1;
      }
      tools::print_line("checked " + to_decimal(result.value().summary.checked));
      tools::print_line("matched " + to_decimal(result.value().summary.matched));
      tools::print_line("missing " + to_decimal(result.value().summary.missing));
      tools::print_line("diverged " + to_decimal(result.value().summary.diverged));
      tools::print_line("extra " + to_decimal(result.value().summary.extra));
      tools::print_line("unavailable " + to_decimal(result.value().summary.unavailable));
      if (!rest.empty()) {
        tools::print_line(std::string("classification ") + to_string(result.value().classification));
      }
      return 0;
    }
    usage();
    return 2;
  }

  // Direct mode.
  DirectRuntime direct;
  PublisherRegistration registration;
  std::string failure;
  if (!build_direct_runtime(options, direct, paths, registration, failure)) {
    tools::print_line("error: " + failure);
    return 1;
  }
  RouteFabricRuntime& runtime = *direct.runtime;

  if (command == "snapshot" || command == "list") {
    const Expected<RouteSnapshotSet> snapshot = runtime.Snapshot();
    if (!snapshot) {
      tools::print_error(snapshot.error());
      return 1;
    }
    tools::print_line("epoch " + snapshot.value().epoch.render());
    tools::print_line("route-count " + to_decimal(snapshot.value().routes.size()));
    tools::print_line("snapshot-id " + snapshot.value().id.render());
    tools::print_line("digest " + snapshot.value().digest.to_hex());
    for (const RouteSnapshot& route : snapshot.value().routes) {
      tools::print_line("  " + route.render_compact());
    }
    return 0;
  }

  if (command == "show" || command == "explain") {
    if (rest.empty()) {
      tools::print_line("error: route " + command + " requires a route identity or destination");
      return 2;
    }
    const Expected<RouteId> route_id = RouteId::parse(rest[0]);
    Expected<RouteSnapshot> snapshot = make_error<RouteSnapshot>(StatusCode::NotFound, "no route");
    Expected<RouteExplanation> explanation = make_error<RouteExplanation>(StatusCode::NotFound, "no route");
    if (route_id) {
      snapshot = runtime.QueryRouteById(route_id.value());
      explanation = runtime.ExplainRouteById(route_id.value());
    } else {
      const Expected<RouteKey> key = build_key(options, rest[0]);
      if (!key) {
        tools::print_error(key.error());
        return 2;
      }
      snapshot = runtime.QueryRoute(key.value());
      explanation = runtime.ExplainRoute(key.value());
    }
    if (command == "show") {
      if (!snapshot) {
        tools::print_error(snapshot.error());
        return 1;
      }
      tools::print_line(snapshot.value().render());
      return 0;
    }
    if (!explanation) {
      tools::print_error(explanation.error());
      return 1;
    }
    tools::print_line(explanation.value().render());
    return 0;
  }

  if (command == "publish") {
    if (rest.empty()) {
      tools::print_line("error: route publish requires a destination");
      return 2;
    }
    const Expected<RouteKey> key = build_key(options, rest[0]);
    if (!key) {
      tools::print_error(key.error());
      return 2;
    }
    std::string binding_failure;
    const RouteBinding binding = build_binding(options, paths, binding_failure);
    if (!binding_failure.empty()) {
      tools::print_line("error: " + binding_failure);
      return 2;
    }
    const Expected<RouteGeneration> expected = parse_expected_generation(options);
    if (!expected) {
      tools::print_error(expected.error());
      return 2;
    }
    PublishRequest request;
    request.publisher = registration.publisher;
    request.worker_boot = registration.worker_boot;
    request.attempt = MutationAttemptId::from_digest(digest128("routefabric.cli.publish", rest[0]));
    request.key = key.value();
    request.binding = binding;
    request.policy_generation = PolicyGeneration::from_value(1);
    request.expected_generation = expected.value();
    request.reason = options.reason;
    const Expected<PublishResult> published = runtime.PublishRoute(request);
    if (!published) {
      tools::print_error(published.error());
      return 1;
    }
    tools::print_line("route-id " + published.value().route.render());
    tools::print_line("generation " + published.value().generation.render());
    tools::print_line("lifecycle " + std::string(to_string(published.value().lifecycle)));
    tools::print_line("applied " + std::string(to_string(published.value().applied)));
    tools::print_line("currentness " + std::string(to_string(published.value().currentness)));
    return 0;
  }

  if (command == "withdraw" || command == "revalidate" || command == "retire" || command == "revoke") {
    if (rest.empty()) {
      tools::print_line("error: route " + command + " requires a route identity");
      return 2;
    }
    const Expected<RouteId> route_id = RouteId::parse(rest[0]);
    if (!route_id) {
      tools::print_error(route_id.error());
      return 2;
    }
    const Expected<RouteGeneration> expected = parse_expected_generation(options);
    if (!expected) {
      tools::print_error(expected.error());
      return 2;
    }
    const MutationAttemptId attempt =
        MutationAttemptId::from_digest(digest128("routefabric.cli.mutation", rest[0] + command));
    Expected<WithdrawOutcome> outcome = make_error<WithdrawOutcome>(StatusCode::Internal, "unhandled");
    if (command == "withdraw") {
      WithdrawRequest request;
      request.publisher = registration.publisher;
      request.worker_boot = registration.worker_boot;
      request.attempt = attempt;
      request.route = route_id.value();
      request.expected_generation = expected.value();
      request.reason = options.reason;
      outcome = runtime.WithdrawRoute(request);
    } else if (command == "revalidate") {
      RevalidateRequest request;
      request.publisher = registration.publisher;
      request.worker_boot = registration.worker_boot;
      request.attempt = attempt;
      request.route = route_id.value();
      request.reason = options.reason;
      outcome = runtime.RevalidateRoute(request);
    } else if (command == "retire") {
      RetireRequest request;
      request.publisher = registration.publisher;
      request.worker_boot = registration.worker_boot;
      request.attempt = attempt;
      request.route = route_id.value();
      request.reason = options.reason;
      outcome = runtime.RetireRoute(request);
    } else {
      RevokeRequest request;
      request.publisher = registration.publisher;
      request.worker_boot = registration.worker_boot;
      request.attempt = attempt;
      request.route = route_id.value();
      request.reason = options.reason;
      outcome = runtime.RevokeRoute(request);
    }
    if (!outcome) {
      tools::print_error(outcome.error());
      return 1;
    }
    tools::print_line("route-id " + outcome.value().route.render());
    tools::print_line("generation " + outcome.value().generation.render());
    tools::print_line("lifecycle " + std::string(to_string(outcome.value().lifecycle)));
    tools::print_line("applied " + std::string(to_string(outcome.value().applied)));
    return 0;
  }

  if (command == "reconcile") {
    if (rest.empty()) {
      const Expected<ReconciliationSummary> summary = runtime.ReconcileAll();
      if (!summary) {
        tools::print_error(summary.error());
        return 1;
      }
      tools::print_line("checked " + to_decimal(summary.value().checked));
      tools::print_line("matched " + to_decimal(summary.value().matched));
      tools::print_line("missing " + to_decimal(summary.value().missing));
      tools::print_line("diverged " + to_decimal(summary.value().diverged));
      tools::print_line("extra " + to_decimal(summary.value().extra));
      tools::print_line("unavailable " + to_decimal(summary.value().unavailable));
      return 0;
    }
    const Expected<RouteId> route_id = RouteId::parse(rest[0]);
    if (!route_id) {
      tools::print_error(route_id.error());
      return 2;
    }
    const Expected<ObservationClass> classification = runtime.ReconcileRoute(route_id.value());
    if (!classification) {
      tools::print_error(classification.error());
      return 1;
    }
    tools::print_line(std::string("classification ") + to_string(classification.value()));
    return 0;
  }

  if (command == "counters") {
    tools::print_line(render_counters(runtime.counters()));
    return 0;
  }

  usage();
  return 2;
}
