#ifndef ROUTEFABRIC_DESTINATION_HPP
#define ROUTEFABRIC_DESTINATION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "routefabric/encoding.hpp"
#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"

namespace routefabric {

// Destination classes. The numeric values are part of the persistence and wire
// representations and must not change once released.
enum class DestinationKind : std::uint8_t {
  Ipv4Prefix = 1,
  Ipv6Prefix = 2,
  FabricEndpoint = 3,
  ServiceEndpoint = 4,
  OverlayEndpoint = 5,
  LogicalDestination = 6,
};

const char* to_string(DestinationKind kind) noexcept;
bool is_prefix_kind(DestinationKind kind) noexcept;

// A canonical route destination.
//
// Canonicalization rules (documented behaviour, enforced by parse and proven by
// tests):
//   * IPv4 and IPv6 destinations always carry an explicit prefix length.
//   * Host bits below the prefix length are cleared: "10.0.0.1/24" canonicalizes
//     to "10.0.0.0/24".
//   * Octets and prefix lengths are strict decimal: no leading zeros, no sign,
//     no whitespace, no out-of-range values.
//   * IPv6 rendering uses lowercase hexadecimal groups with the longest run of
//     two or more zero groups compressed (leftmost run wins on a tie). IPv4
//     compatible forms are stored and rendered as pure hexadecimal groups.
//   * Endpoint destinations are bounded tokens from [A-Za-z0-9._-] starting
//     with an alphanumeric character.
//
// Destination identity (DestinationId) is the content digest of the canonical
// encoding, so two textually different spellings of the same destination share
// one identity.
class Destination {
 public:
  Destination() = default;

  static Expected<Destination> parse_ipv4_prefix(std::string_view text);
  static Expected<Destination> parse_ipv6_prefix(std::string_view text);
  static Expected<Destination> parse_endpoint(DestinationKind kind, std::string_view token,
                                              std::size_t max_token_length);

  // Auto-detecting parser used by the CLI and by the protocol layer:
  //   * text containing '/' and a '.' but no ':'  -> IPv4 prefix
  //   * text containing '/' and ':'               -> IPv6 prefix
  //   * "ipv4:<text>" / "ipv6:<text>" / "fabric:<token>" / "service:<token>" /
  //     "overlay:<token>" / "logical:<token>"     -> explicit kind
  static Expected<Destination> parse(std::string_view text, std::size_t max_token_length);

  DestinationKind kind() const noexcept { return kind_; }
  bool is_valid() const noexcept { return valid_; }
  bool is_prefix() const noexcept { return valid_ && is_prefix_kind(kind_); }
  std::uint8_t prefix_length() const noexcept { return prefix_length_; }
  std::size_t address_size() const noexcept;
  std::span<const std::uint8_t> address() const noexcept;
  const std::string& token() const noexcept { return token_; }

  std::string render() const;

  DestinationId id() const;

  // Authority-scope coverage test. This is *not* route lookup and is never used
  // to select a route: it answers whether a publisher scoped to this
  // destination is allowed to govern the supplied destination.
  bool scope_covers(const Destination& candidate) const noexcept;

  void write(ByteWriter& writer) const;
  static bool read(ByteReader& reader, std::size_t max_token_length, Destination& out);

  friend bool operator==(const Destination&, const Destination&) = default;
  friend auto operator<=>(const Destination&, const Destination&) = default;

 private:
  DestinationKind kind_ = DestinationKind::LogicalDestination;
  bool valid_ = false;
  std::array<std::uint8_t, 16> address_{};
  std::uint8_t prefix_length_ = 0;
  std::string token_;
};

// Bounded, strict unsigned decimal parser for a single octet or prefix length.
bool parse_decimal_field(std::string_view text, std::uint32_t max_value, std::uint32_t& out) noexcept;

// Strict IPv6 textual parser producing the 16 address bytes.
bool parse_ipv6_address(std::string_view text, std::array<std::uint8_t, 16>& out) noexcept;

}  // namespace routefabric

#endif  // ROUTEFABRIC_DESTINATION_HPP
