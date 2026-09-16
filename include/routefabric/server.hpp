#ifndef ROUTEFABRIC_SERVER_HPP
#define ROUTEFABRIC_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "routefabric/backend.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/runtime.hpp"

namespace routefabric {

struct ServerConfig {
  // Literal IPv4 or IPv6 address to bind. The default keeps the coordinator on
  // the loopback interface.
  std::string bind_address = "127.0.0.1";
  // Zero selects an ephemeral port, reported through RouteFabricServer::port().
  std::uint16_t port = 0;
  Limits limits;
};

struct ServerCounters {
  std::uint64_t sessions_accepted = 0;
  std::uint64_t sessions_rejected = 0;
  std::uint64_t sessions_closed = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t requests_handled = 0;
  std::uint64_t malformed_frames = 0;
  std::uint64_t integrity_failures = 0;
  std::uint64_t oversized_frames = 0;
  std::uint64_t protocol_errors = 0;
};

// Coordinator-side network front end for one RouteFabricRuntime.
//
// Process model: one accept thread plus one thread per session. The transport is
// plain TCP with a non-cryptographic integrity check; the default binding is
// loopback and no cryptographic authentication is implemented or claimed.
//
// Session lifecycle: a session that registered a publisher fences that
// publisher's worker boot when the connection ends, but only if the runtime
// still holds that exact boot as the live registration. A reincarnated worker
// that already re-registered with a fresh boot is therefore never disturbed by a
// lingering session teardown.
class RouteFabricServer {
 public:
  RouteFabricServer(ServerConfig config, RouteFabricRuntime* runtime, IRouteProgrammingBackend* backend);
  ~RouteFabricServer();

  RouteFabricServer(const RouteFabricServer&) = delete;
  RouteFabricServer& operator=(const RouteFabricServer&) = delete;

  Status Start();
  // Stops accepting, unblocks and joins every session thread, and returns only
  // when no session thread remains.
  void Stop();

  bool running() const;
  std::uint16_t port() const;
  std::size_t session_count() const;
  ServerCounters counters() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_SERVER_HPP
