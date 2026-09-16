#ifndef ROUTEFABRIC_LIMITS_HPP
#define ROUTEFABRIC_LIMITS_HPP

#include <cstddef>

#include "routefabric/error.hpp"

namespace routefabric {

// Every field below is consulted by the operation it names. Limits are part of
// the runtime configuration and are validated at construction time.
struct Limits {
  // Maximum number of authoritative route records held by the runtime.
  std::size_t max_routes = 1'000'000;
  // Maximum number of distinct routing namespaces accepted in route keys.
  std::size_t max_routing_namespaces = 4096;
  // Maximum size of a single wire frame, header included.
  std::size_t max_frame_bytes = 1u << 20;
  // Maximum length of a token destination (fabric/service/overlay/logical).
  std::size_t max_destination_chars = 256;
  // Maximum retained history entries per route lineage.
  std::size_t max_history_entries_per_route = 32;
  // Maximum simultaneous outstanding (dispatched but unresolved) programming
  // operations.
  std::size_t max_outstanding_programming = 4096;
  // Maximum number of registered publishers.
  std::size_t max_publishers = 4096;
  // Maximum concurrent accepted sessions on a coordinator.
  std::size_t max_sessions = 128;
  // Maximum number of routes a single snapshot may contain.
  std::size_t max_snapshot_routes = 1'000'000;
  // Maximum number of entries in one batch publication.
  std::size_t max_batch_mutations = 1024;
  // Maximum number of scope entries bound to one publisher registration.
  std::size_t max_publisher_scopes = 16;
  // Maximum accepted persistence image size in bytes.
  std::size_t max_persist_bytes = 1u << 30;

  // Rejects incoherent configurations (for example a zero bound where a bound
  // is required).
  Status validate() const;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_LIMITS_HPP
