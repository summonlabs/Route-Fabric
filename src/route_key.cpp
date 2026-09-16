#include "routefabric/route_key.hpp"

namespace routefabric {

const char* to_string(RouteClass route_class) noexcept {
  switch (route_class) {
    case RouteClass::Unicast:
      return "unicast";
    case RouteClass::Host:
      return "host";
    case RouteClass::Service:
      return "service";
    case RouteClass::Overlay:
      return "overlay";
    case RouteClass::Logical:
      return "logical";
  }
  return "unknown";
}

bool parse_route_class(std::string_view text, RouteClass& out) noexcept {
  if (text == "unicast") {
    out = RouteClass::Unicast;
    return true;
  }
  if (text == "host") {
    out = RouteClass::Host;
    return true;
  }
  if (text == "service") {
    out = RouteClass::Service;
    return true;
  }
  if (text == "overlay") {
    out = RouteClass::Overlay;
    return true;
  }
  if (text == "logical") {
    out = RouteClass::Logical;
    return true;
  }
  return false;
}

bool RouteKey::is_valid() const noexcept {
  if (!fabric.is_valid() || !routing_namespace.is_valid() || !destination.is_valid()) {
    return false;
  }
  switch (route_class) {
    case RouteClass::Unicast:
    case RouteClass::Service:
    case RouteClass::Overlay:
    case RouteClass::Logical:
      break;
    case RouteClass::Host: {
      if (!destination.is_prefix()) {
        return false;
      }
      const std::size_t bits = destination.address_size() * 8;
      if (static_cast<std::size_t>(destination.prefix_length()) != bits) {
        return false;  // a host route binds one address
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

std::string RouteKey::render() const {
  std::string out;
  out += fabric.render();
  out.push_back('/');
  out += routing_namespace.render();
  out.push_back('/');
  out += to_string(route_class);
  out.push_back('/');
  out += destination.render();
  return out;
}

void RouteKey::write(ByteWriter& writer) const {
  fabric.write(writer);
  routing_namespace.write(writer);
  writer.u8(static_cast<std::uint8_t>(route_class));
  destination.write(writer);
}

bool RouteKey::read(ByteReader& reader, std::size_t max_destination_token, RouteKey& out) {
  RouteKey key;
  std::uint8_t route_class = 0;
  if (!FabricId::read(reader, key.fabric) || !RoutingNamespace::read(reader, key.routing_namespace)) {
    return false;
  }
  if (!reader.u8(route_class)) {
    return false;
  }
  switch (static_cast<RouteClass>(route_class)) {
    case RouteClass::Unicast:
    case RouteClass::Host:
    case RouteClass::Service:
    case RouteClass::Overlay:
    case RouteClass::Logical:
      key.route_class = static_cast<RouteClass>(route_class);
      break;
    default:
      return false;
  }
  if (!Destination::read(reader, max_destination_token, key.destination)) {
    return false;
  }
  if (!key.is_valid()) {
    return false;
  }
  out = std::move(key);
  return true;
}

RouteId derive_route_id(const RouteKey& key) {
  ByteWriter writer;
  writer.raw("routefabric.route-id.v1");
  key.write(writer);
  return RouteId::from_digest(digest128("routefabric.route-id", writer.buffer()));
}

RouteId derive_lineage_route_id(const RouteKey& key, const MutationAttemptId& lineage_mint) {
  ByteWriter writer;
  writer.raw("routefabric.lineage-id.v1");
  key.write(writer);
  lineage_mint.write(writer);
  return RouteId::from_digest(digest128("routefabric.lineage-id", writer.buffer()));
}

}  // namespace routefabric
