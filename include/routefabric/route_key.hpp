#ifndef ROUTEFABRIC_ROUTE_KEY_HPP
#define ROUTEFABRIC_ROUTE_KEY_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "routefabric/destination.hpp"
#include "routefabric/encoding.hpp"
#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"

namespace routefabric {

// Route class. Part of the semantic route key.
enum class RouteClass : std::uint8_t {
  Unicast = 1,
  Host = 2,
  Service = 3,
  Overlay = 4,
  Logical = 5,
};

const char* to_string(RouteClass route_class) noexcept;
bool parse_route_class(std::string_view text, RouteClass& out) noexcept;

// The semantic route key: the identity of the authoritative route slot.
//
// The key deliberately excludes publisher, provenance, timestamps, next hop,
// path binding and generation. Two publishers that target the same key collide
// through governance instead of silently creating two route identities.
struct RouteKey {
  FabricId fabric;
  RoutingNamespace routing_namespace;
  Destination destination;
  RouteClass route_class = RouteClass::Unicast;

  bool is_valid() const noexcept;

  // Canonical, script-friendly rendering:
  //   <fabric>/<namespace>/<route-class>/<destination>
  std::string render() const;

  void write(ByteWriter& writer) const;
  static bool read(ByteReader& reader, std::size_t max_destination_token, RouteKey& out);

  friend bool operator==(const RouteKey&, const RouteKey&) = default;
  friend auto operator<=>(const RouteKey&, const RouteKey&) = default;
};

// Route identity is derived deterministically from the route key, so it is
// stable across every semantic mutation of the same route and reproducible
// across process restarts. A different key always produces a different identity.
RouteId derive_route_id(const RouteKey& key);

// Explicitly minted lineage identity. Used only for administrative recreation of
// a key whose previous lineage is retired.
RouteId derive_lineage_route_id(const RouteKey& key, const MutationAttemptId& lineage_mint);

}  // namespace routefabric

#endif  // ROUTEFABRIC_ROUTE_KEY_HPP
