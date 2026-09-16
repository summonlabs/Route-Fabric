#include "routefabric/destination.hpp"

#include <vector>

namespace routefabric {
namespace {

Expected<Destination> ipv4_error(std::string_view detail) {
  return make_error<Destination>(StatusCode::InvalidArgument, std::string("invalid IPv4 destination: ") + std::string(detail));
}

Expected<Destination> ipv6_error(std::string_view detail) {
  return make_error<Destination>(StatusCode::InvalidArgument, std::string("invalid IPv6 destination: ") + std::string(detail));
}

void mask_host_bits(std::array<std::uint8_t, 16>& address, std::uint8_t prefix_length) {
  for (std::uint8_t bit = prefix_length; bit < 128; ++bit) {
    const std::size_t index = static_cast<std::size_t>(bit / 8);
    const std::uint8_t mask = static_cast<std::uint8_t>(0x80u >> (bit % 8));
    address[index] = static_cast<std::uint8_t>(address[index] & static_cast<std::uint8_t>(~mask));
  }
}

bool same_network(const Destination& scope, const Destination& candidate) noexcept {
  if (scope.kind() != candidate.kind()) {
    return false;
  }
  if (scope.prefix_length() > candidate.prefix_length()) {
    return false;
  }
  const std::uint8_t bits = scope.prefix_length();
  const std::size_t whole_bytes = static_cast<std::size_t>(bits / 8);
  const std::uint8_t remainder = static_cast<std::uint8_t>(bits % 8);
  const auto a = scope.address();
  const auto b = candidate.address();
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < whole_bytes; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  if (remainder != 0) {
    const std::uint8_t mask = static_cast<std::uint8_t>(0xFFu << (8 - remainder));
    if ((a[whole_bytes] & mask) != (b[whole_bytes] & mask)) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* to_string(DestinationKind kind) noexcept {
  switch (kind) {
    case DestinationKind::Ipv4Prefix:
      return "ipv4-prefix";
    case DestinationKind::Ipv6Prefix:
      return "ipv6-prefix";
    case DestinationKind::FabricEndpoint:
      return "fabric-endpoint";
    case DestinationKind::ServiceEndpoint:
      return "service-endpoint";
    case DestinationKind::OverlayEndpoint:
      return "overlay-endpoint";
    case DestinationKind::LogicalDestination:
      return "logical-destination";
  }
  return "unknown";
}

bool is_prefix_kind(DestinationKind kind) noexcept {
  return kind == DestinationKind::Ipv4Prefix || kind == DestinationKind::Ipv6Prefix;
}

bool parse_decimal_field(std::string_view text, std::uint32_t max_value, std::uint32_t& out) noexcept {
  if (text.empty() || text.size() > 3) {
    return false;
  }
  if (text.size() > 1 && text.front() == '0') {
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = (value * 10u) + static_cast<std::uint32_t>(c - '0');
    if (value > max_value) {
      return false;
    }
  }
  out = value;
  return true;
}

bool parse_ipv6_address(std::string_view text, std::array<std::uint8_t, 16>& out) noexcept {
  out.fill(0);
  if (text.empty() || text.find(':') == std::string_view::npos) {
    return false;
  }
  if (text.find('%') != std::string_view::npos) {
    return false;  // zone identifiers are not part of a canonical fabric destination
  }

  std::vector<std::uint16_t> groups;
  groups.reserve(8);
  std::size_t double_colon = std::string_view::npos;
  std::size_t index = 0;

  while (index < text.size()) {
    if (text[index] == ':') {
      if (index + 1 < text.size() && text[index + 1] == ':') {
        if (double_colon != std::string_view::npos) {
          return false;  // at most one "::"
        }
        double_colon = groups.size();
        index += 2;
        if (index == text.size()) {
          break;
        }
        continue;
      }
      if (index == 0 || groups.empty()) {
        return false;  // leading single colon
      }
      ++index;
      if (index == text.size()) {
        return false;  // trailing single colon
      }
      continue;
    }

    const std::size_t start = index;
    while (index < text.size() && text[index] != ':') {
      ++index;
    }
    const std::string_view field = text.substr(start, index - start);
    if (field.empty()) {
      return false;
    }

    // An embedded IPv4 tail is only accepted as the final 32 bits.
    if (field.find('.') != std::string_view::npos) {
      if (index != text.size()) {
        return false;
      }
      std::size_t part_start = 0;
      std::array<std::uint32_t, 4> octets{};
      int octet_index = 0;
      for (std::size_t i = 0; i <= field.size(); ++i) {
        if (i == field.size() || field[i] == '.') {
          if (octet_index >= 4) {
            return false;
          }
          std::uint32_t value = 0;
          if (!parse_decimal_field(field.substr(part_start, i - part_start), 255, value)) {
            return false;
          }
          octets[static_cast<std::size_t>(octet_index)] = value;
          ++octet_index;
          part_start = i + 1;
        }
      }
      if (octet_index != 4) {
        return false;
      }
      groups.push_back(static_cast<std::uint16_t>((octets[0] << 8) | octets[1]));
      groups.push_back(static_cast<std::uint16_t>((octets[2] << 8) | octets[3]));
      break;
    }

    if (field.size() > 4) {
      return false;
    }
    std::uint16_t value = 0;
    for (const char c : field) {
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
    }
    groups.push_back(value);
    if (groups.size() > 8) {
      return false;
    }
  }

  if (groups.size() > 8) {
    return false;
  }
  if (double_colon == std::string_view::npos) {
    if (groups.size() != 8) {
      return false;
    }
  } else if (groups.size() >= 8) {
    return false;  // "::" must stand for at least one zero group
  }

  std::vector<std::uint16_t> expanded(8, 0);
  if (double_colon == std::string_view::npos) {
    expanded = groups;
  } else {
    const std::size_t tail = groups.size() - double_colon;
    for (std::size_t i = 0; i < double_colon; ++i) {
      expanded[i] = groups[i];
    }
    for (std::size_t i = 0; i < tail; ++i) {
      expanded[8 - tail + i] = groups[double_colon + i];
    }
  }

  for (std::size_t i = 0; i < 8; ++i) {
    out[2 * i] = static_cast<std::uint8_t>((expanded[i] >> 8) & 0xFFu);
    out[(2 * i) + 1] = static_cast<std::uint8_t>(expanded[i] & 0xFFu);
  }
  return true;
}

Expected<Destination> Destination::parse_ipv4_prefix(std::string_view text) {
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos || text.find('/', slash + 1) != std::string_view::npos) {
    return ipv4_error("expected exactly one '/' separating address and prefix length");
  }
  const std::string_view address_text = text.substr(0, slash);
  const std::string_view prefix_text = text.substr(slash + 1);

  std::array<std::uint32_t, 4> octets{};
  std::size_t start = 0;
  int octet_index = 0;
  for (std::size_t i = 0; i <= address_text.size(); ++i) {
    if (i == address_text.size() || address_text[i] == '.') {
      if (octet_index >= 4) {
        return ipv4_error("too many octets");
      }
      std::uint32_t value = 0;
      if (!parse_decimal_field(address_text.substr(start, i - start), 255, value)) {
        return ipv4_error("each octet must be a decimal value in [0,255] without leading zeros");
      }
      octets[static_cast<std::size_t>(octet_index)] = value;
      ++octet_index;
      start = i + 1;
    }
  }
  if (octet_index != 4) {
    return ipv4_error("expected four octets");
  }
  if (address_text.find_first_not_of("0123456789.") != std::string_view::npos) {
    return ipv4_error("unexpected character in address");
  }

  std::uint32_t prefix = 0;
  if (!parse_decimal_field(prefix_text, 32, prefix)) {
    return ipv4_error("prefix length must be a decimal value in [0,32] without leading zeros");
  }

  Destination destination;
  destination.kind_ = DestinationKind::Ipv4Prefix;
  destination.valid_ = true;
  for (std::size_t i = 0; i < 4; ++i) {
    destination.address_[i] = static_cast<std::uint8_t>(octets[i]);
  }
  destination.prefix_length_ = static_cast<std::uint8_t>(prefix);
  mask_host_bits(destination.address_, destination.prefix_length_);
  return destination;
}

Expected<Destination> Destination::parse_ipv6_prefix(std::string_view text) {
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos || text.find('/', slash + 1) != std::string_view::npos) {
    return ipv6_error("expected exactly one '/' separating address and prefix length");
  }
  const std::string_view address_text = text.substr(0, slash);
  const std::string_view prefix_text = text.substr(slash + 1);

  std::array<std::uint8_t, 16> address{};
  if (!parse_ipv6_address(address_text, address)) {
    return ipv6_error("malformed address");
  }
  std::uint32_t prefix = 0;
  if (!parse_decimal_field(prefix_text, 128, prefix)) {
    return ipv6_error("prefix length must be a decimal value in [0,128] without leading zeros");
  }

  Destination destination;
  destination.kind_ = DestinationKind::Ipv6Prefix;
  destination.valid_ = true;
  destination.address_ = address;
  destination.prefix_length_ = static_cast<std::uint8_t>(prefix);
  mask_host_bits(destination.address_, destination.prefix_length_);
  return destination;
}

Expected<Destination> Destination::parse_endpoint(DestinationKind kind, std::string_view token, std::size_t max_token_length) {
  if (is_prefix_kind(kind)) {
    return make_error<Destination>(StatusCode::InvalidArgument, "endpoint parser requires an endpoint destination kind");
  }
  if (!is_valid_token_text(token, max_token_length)) {
    return make_error<Destination>(StatusCode::InvalidArgument,
                                   std::string("endpoint destination token must be 1..") + to_decimal(max_token_length) +
                                       " characters from [A-Za-z0-9._-] starting with an alphanumeric character");
  }
  Destination destination;
  destination.kind_ = kind;
  destination.valid_ = true;
  destination.token_ = std::string(token);
  return destination;
}

Expected<Destination> Destination::parse(std::string_view text, std::size_t max_token_length) {
  if (text.empty()) {
    return make_error<Destination>(StatusCode::InvalidArgument, "destination must not be empty");
  }
  const std::size_t colon = text.find(':');
  const bool known_scheme =
      colon != std::string_view::npos &&
      (text.substr(0, colon) == "ipv4" || text.substr(0, colon) == "ipv6" ||
       text.substr(0, colon) == "fabric" || text.substr(0, colon) == "service" ||
       text.substr(0, colon) == "overlay" || text.substr(0, colon) == "logical");
  if (known_scheme) {
    const std::string_view scheme = text.substr(0, colon);
    const std::string_view rest = text.substr(colon + 1);
    if (scheme == "ipv4") {
      return parse_ipv4_prefix(rest);
    }
    if (scheme == "ipv6") {
      return parse_ipv6_prefix(rest);
    }
    if (scheme == "fabric") {
      return parse_endpoint(DestinationKind::FabricEndpoint, rest, max_token_length);
    }
    if (scheme == "service") {
      return parse_endpoint(DestinationKind::ServiceEndpoint, rest, max_token_length);
    }
    if (scheme == "overlay") {
      return parse_endpoint(DestinationKind::OverlayEndpoint, rest, max_token_length);
    }
    if (scheme == "logical") {
      return parse_endpoint(DestinationKind::LogicalDestination, rest, max_token_length);
    }
  }
  if (text.find('/') != std::string_view::npos) {
    if (text.find(':') != std::string_view::npos) {
      return parse_ipv6_prefix(text);
    }
    return parse_ipv4_prefix(text);
  }
  return make_error<Destination>(StatusCode::InvalidArgument,
                                 "destination requires an explicit kind prefix (fabric:, service:, overlay:, logical:) "
                                 "or an IPv4/IPv6 prefix with a length");
}

std::size_t Destination::address_size() const noexcept {
  if (kind_ == DestinationKind::Ipv4Prefix) {
    return 4;
  }
  if (kind_ == DestinationKind::Ipv6Prefix) {
    return 16;
  }
  return 0;
}

std::span<const std::uint8_t> Destination::address() const noexcept {
  return std::span<const std::uint8_t>(address_.data(), address_size());
}

std::string Destination::render() const {
  if (!valid_) {
    return "<invalid-destination>";
  }
  if (kind_ == DestinationKind::Ipv4Prefix) {
    std::string out;
    for (std::size_t i = 0; i < 4; ++i) {
      if (i != 0) {
        out.push_back('.');
      }
      out += to_decimal(address_[i]);
    }
    out.push_back('/');
    out += to_decimal(prefix_length_);
    return out;
  }
  if (kind_ == DestinationKind::Ipv6Prefix) {
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < 8; ++i) {
      groups[i] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(address_[2 * i]) << 8) |
                                             static_cast<std::uint16_t>(address_[(2 * i) + 1]));
    }
    std::size_t best_start = 8;
    std::size_t best_length = 0;
    std::size_t current_start = 0;
    std::size_t current_length = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      if (groups[i] == 0) {
        if (current_length == 0) {
          current_start = i;
        }
        ++current_length;
        if (current_length > best_length) {
          best_length = current_length;
          best_start = current_start;
        }
      } else {
        current_length = 0;
      }
    }
    if (best_length < 2) {
      best_start = 8;
      best_length = 0;
    }
    std::string out;
    for (std::size_t i = 0; i < 8; ++i) {
      if (best_length != 0 && i == best_start) {
        out += "::";
        i += best_length - 1;
        continue;
      }
      if (!out.empty() && out.back() != ':') {
        out.push_back(':');
      }
      static constexpr char kHex[] = "0123456789abcdef";
      const std::uint16_t group = groups[i];
      bool started = false;
      for (int shift = 12; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<std::uint16_t>((group >> shift) & 0x0Fu);
        if (nibble != 0 || started || shift == 0) {
          out.push_back(kHex[nibble]);
          started = true;
        }
      }
    }
    out.push_back('/');
    out += to_decimal(prefix_length_);
    return out;
  }
  std::string out(to_string(kind_));
  out.push_back(':');
  out += token_;
  return out;
}

DestinationId Destination::id() const {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(kind_));
  writer.u8(static_cast<std::uint8_t>(prefix_length_));
  writer.raw(address());
  writer.string(token_);
  return DestinationId::from_digest(digest128("routefabric.destination.v1", writer.buffer()));
}

bool Destination::scope_covers(const Destination& candidate) const noexcept {
  if (!valid_ || !candidate.valid_) {
    return false;
  }
  if (kind_ != candidate.kind_) {
    return false;
  }
  if (is_prefix_kind(kind_)) {
    return same_network(*this, candidate);
  }
  return token_ == candidate.token_;
}

void Destination::write(ByteWriter& writer) const {
  writer.u8(static_cast<std::uint8_t>(kind_));
  writer.u8(prefix_length_);
  writer.raw(std::span<const std::uint8_t>(address_.data(), 16));
  writer.string(token_);
}

bool Destination::read(ByteReader& reader, std::size_t max_token_length, Destination& out) {
  std::uint8_t kind = 0;
  std::uint8_t prefix = 0;
  std::span<const std::uint8_t> raw;
  std::string token;
  if (!reader.u8(kind) || !reader.u8(prefix)) {
    return false;
  }
  const auto kind_value = static_cast<DestinationKind>(kind);
  switch (kind_value) {
    case DestinationKind::Ipv4Prefix:
    case DestinationKind::Ipv6Prefix:
    case DestinationKind::FabricEndpoint:
    case DestinationKind::ServiceEndpoint:
    case DestinationKind::OverlayEndpoint:
    case DestinationKind::LogicalDestination:
      break;
    default:
      return false;  // malformed enum value
  }
  if (!reader.raw(16, raw)) {
    return false;
  }
  if (!reader.string(max_token_length, token)) {
    return false;
  }

  Destination destination;
  destination.kind_ = kind_value;
  destination.valid_ = true;
  for (std::size_t i = 0; i < 16; ++i) {
    destination.address_[i] = raw[i];
  }
  destination.prefix_length_ = prefix;
  destination.token_ = std::move(token);

  if (is_prefix_kind(kind_value)) {
    const std::uint8_t max_prefix = kind_value == DestinationKind::Ipv4Prefix ? 32 : 128;
    if (prefix > max_prefix) {
      return false;
    }
    if (!destination.token_.empty()) {
      return false;  // prefix destinations must not carry a token
    }
    if (kind_value == DestinationKind::Ipv4Prefix) {
      for (std::size_t i = 4; i < 16; ++i) {
        if (destination.address_[i] != 0) {
          return false;
        }
      }
    }
    std::array<std::uint8_t, 16> canonical = destination.address_;
    mask_host_bits(canonical, prefix);
    if (canonical != destination.address_) {
      return false;  // non-canonical encoding (host bits set)
    }
  } else {
    if (prefix != 0) {
      return false;
    }
    if (!is_valid_token_text(destination.token_, max_token_length)) {
      return false;
    }
    for (const std::uint8_t byte : destination.address_) {
      if (byte != 0) {
        return false;
      }
    }
  }
  out = std::move(destination);
  return true;
}

}  // namespace routefabric
