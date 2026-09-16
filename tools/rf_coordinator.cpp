// Route Fabric coordinator process.
//
// Runs one authoritative RouteFabricRuntime behind a TCP front end. The process
// runs until it is terminated or, when explicitly requested, until a shutdown
// line arrives on standard input.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/path_authority.hpp"
#include "routefabric/real_backend.hpp"
#include "routefabric/runtime.hpp"
#include "routefabric/server.hpp"
#include "routefabric/version.hpp"
#include "tools_common.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Options {
  std::string store;
  std::string durability = "none";
  std::string bind = "127.0.0.1";
  std::uint16_t port = 0;
  std::string fabric = "fabric";
  std::string backend = "synthetic";
  std::string path_file;
  std::string exit_file;
  std::string id_seed;
  std::string epoch_policy = "resume";
  bool shutdown_on_stdin = false;
  bool reconcile_on_start = true;
};

void usage() {
  routefabric::tools::print_line("usage: rf_coordinator [options]");
  routefabric::tools::print_line("  --store <path>            durable store base path");
  routefabric::tools::print_line("  --durability <mode>       none | snapshot | journal (default none)");
  routefabric::tools::print_line("  --bind <address>          literal IPv4/IPv6 bind address (default 127.0.0.1)");
  routefabric::tools::print_line("  --port <n>                TCP port, 0 selects an ephemeral port");
  routefabric::tools::print_line("  --fabric <id>             authority domain (default fabric)");
  routefabric::tools::print_line("  --backend <kind>          synthetic | real (real is read-only evidence)");
  routefabric::tools::print_line("  --paths <file>            path authority bootstrap file");
  routefabric::tools::print_line("  --seed <n>                deterministic identity seed");
  routefabric::tools::print_line("  --epoch-policy <policy>   resume | advance (a restarted coordinator takes the next epoch)");
  routefabric::tools::print_line("  --no-reconcile            skip startup reconciliation");
  routefabric::tools::print_line("  --exit-file <path>        exit cleanly when this file appears");
  routefabric::tools::print_line("  --shutdown-on-stdin       exit when standard input reports shutdown or ends");
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--help") {
      usage();
      return false;
    }
    if (argument == "--shutdown-on-stdin") {
      options.shutdown_on_stdin = true;
      continue;
    }
    if (argument == "--no-reconcile") {
      options.reconcile_on_start = false;
      continue;
    }
    if (!has_value) {
      routefabric::tools::print_line("error: missing value for " + argument);
      return false;
    }
    const std::string value = argv[++i];
    if (argument == "--store") {
      options.store = value;
    } else if (argument == "--durability") {
      options.durability = value;
    } else if (argument == "--bind") {
      options.bind = value;
    } else if (argument == "--port") {
      std::uint64_t parsed = 0;
      if (!routefabric::parse_u64_decimal(value, 65535, parsed)) {
        routefabric::tools::print_line("error: --port must be a decimal port number");
        return false;
      }
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--fabric") {
      options.fabric = value;
    } else if (argument == "--backend") {
      options.backend = value;
    } else if (argument == "--paths") {
      options.path_file = value;
    } else if (argument == "--exit-file") {
      options.exit_file = value;
    } else if (argument == "--seed") {
      options.id_seed = value;
    } else if (argument == "--epoch-policy") {
      options.epoch_policy = value;
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

  const Expected<FabricId> fabric = FabricId::parse(options.fabric);
  if (!fabric) {
    tools::print_error(fabric.error());
    return 2;
  }
  Durability durability = Durability::None;
  if (!parse_durability(options.durability, durability)) {
    tools::print_line("error: --durability must be none, snapshot or journal");
    return 2;
  }
  if (durability != Durability::None && options.store.empty()) {
    tools::print_line("error: durability requires --store");
    return 2;
  }
  if (durability == Durability::None && !options.store.empty()) {
    tools::print_line("error: --store requires --durability snapshot or journal");
    return 2;
  }

  std::unique_ptr<SyntheticProgrammingBackend> synthetic;
  std::unique_ptr<WindowsRouteTableBackend> real;
  IRouteProgrammingBackend* backend = nullptr;
  const bool use_real = options.backend == "real";
  if (use_real) {
    if (!WindowsRouteTableBackend::is_supported()) {
      tools::print_line("error: the host routing table adapter is not supported on this platform");
      return 2;
    }
    real = std::make_unique<WindowsRouteTableBackend>();
    backend = real.get();
  } else if (options.backend == "synthetic") {
    const Expected<BackendId> backend_id = BackendId::parse("synthetic-programming");
    synthetic = std::make_unique<SyntheticProgrammingBackend>(backend_id.value());
    backend = synthetic.get();
  } else {
    tools::print_line("error: --backend must be synthetic or real");
    return 2;
  }

  SyntheticPathAuthority path_authority;
  if (!options.path_file.empty()) {
    std::ifstream input(options.path_file);
    if (!input) {
      tools::print_line("error: cannot read the path authority bootstrap file");
      return 2;
    }
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::string path_text;
      std::string generation_text;
      std::string state_text;
      std::string extra;
      std::istringstream stream(line);
      if (!(stream >> path_text >> generation_text >> state_text) || (stream >> extra)) {
        tools::print_line("error: malformed path authority line: " + line);
        return 2;
      }
      const Expected<PathId> path = PathId::parse(path_text);
      const Expected<PathAuthorityGeneration> generation = PathAuthorityGeneration::parse(generation_text);
      PathAuthorization state = PathAuthorization::Usable;
      if (!path || !generation || !parse_path_authorization(state_text, state)) {
        tools::print_line("error: malformed path authority entry: " + line);
        return 2;
      }
      const Status status = path_authority.SetPath(path.value(), generation.value(), state);
      if (!status) {
        tools::print_error(status.error());
        return 2;
      }
    }
  }

  RuntimeConfig config;
  config.fabric = fabric.value();
  config.durability = durability;
  config.store_path = options.store;
  if (!options.id_seed.empty()) {
    std::uint64_t seed = 0;
    if (!parse_u64_decimal(options.id_seed, 0xFFFFFFFFFFFFFFFFull, seed)) {
      tools::print_line("error: --seed must be a decimal integer");
      return 2;
    }
    config.id_seed = seed;
  }

  RouteFabricRuntime runtime(config, backend, &path_authority);
  const Status opened = runtime.Open();
  if (!opened) {
    tools::print_error(opened.error());
    return 1;
  }
  if (options.epoch_policy == "advance") {
    const Expected<CoordinatorEpoch> advanced = runtime.AdvanceEpoch();
    if (!advanced) {
      tools::print_error(advanced.error());
      return 1;
    }
  } else if (options.epoch_policy != "resume") {
    tools::print_line("error: --epoch-policy must be resume or advance");
    return 2;
  }
  if (options.reconcile_on_start) {
    const Expected<ReconciliationSummary> summary = runtime.ReconcileAll();
    if (!summary) {
      tools::print_error(summary.error());
      return 1;
    }
  }

  ServerConfig server_config;
  server_config.bind_address = options.bind;
  server_config.port = options.port;
  RouteFabricServer server(server_config, &runtime, backend);
  const Status started = server.Start();
  if (!started) {
    tools::print_error(started.error());
    return 1;
  }

  const BackendCapabilities capabilities = backend->capabilities();
  tools::print_line(std::string("route-fabric-coordinator ") + std::string(kVersionString));
  tools::print_line("wire-protocol " + to_decimal(kWireProtocolVersion));
  tools::print_line("persistence-format " + to_decimal(kPersistenceFormatVersion));
  tools::print_line("fabric " + fabric.value().render());
  tools::print_line("backend " + backend->id().render() + (use_real ? " REAL" : " SYNTHETIC") +
                    (capabilities.read_only ? " read-only" : " programmable"));
  tools::print_line("path-authority synthetic");
  tools::print_line("durability " + std::string(to_string(durability)));
  tools::print_line("store " + (options.store.empty() ? std::string("none") : options.store));
  tools::print_line("epoch " + runtime.epoch().render());
  tools::print_line("listening " + options.bind + " " + to_decimal(server.port()));
  tools::print_line("ready");
  tools::flush_output();

  std::promise<void> shutdown;
  std::future<void> shutdown_future = shutdown.get_future();
  std::atomic<bool> watcher_stop{false};
  std::thread stdin_thread;
  if (options.shutdown_on_stdin) {
    stdin_thread = std::thread([&shutdown]() {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (line == "shutdown") {
          break;
        }
      }
      shutdown.set_value();
    });
  }
  std::thread exit_watcher;
  if (!options.exit_file.empty()) {
    exit_watcher = std::thread([&shutdown, &watcher_stop, &options]() {
      std::error_code error;
      while (!watcher_stop.load()) {
        if (std::filesystem::exists(options.exit_file, error)) {
          shutdown.set_value();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }

  shutdown_future.wait();
  watcher_stop.store(true);
  if (stdin_thread.joinable()) {
    stdin_thread.detach();
  }
  if (exit_watcher.joinable()) {
    exit_watcher.join();
  }
  server.Stop();
  tools::print_line("shutdown complete");
  tools::flush_output();
  return 0;
}
