#ifndef ROUTEFABRIC_IDS_HPP
#define ROUTEFABRIC_IDS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "routefabric/encoding.hpp"
#include "routefabric/error.hpp"
#include "routefabric/hash.hpp"

namespace routefabric {

// ---------------------------------------------------------------------------
// Identity primitives
//
// Route Fabric uses strongly typed identities: a RouteId can never be passed
// where a PathId is expected, and a generation counter can never be silently
// substituted for an epoch. Every identity type can be parsed, rendered,
// encoded, decoded and ordered deterministically, and every decoder rejects
// malformed or zero-valued encodings.
// ---------------------------------------------------------------------------

// Shared validation helpers (defined in src/ids.cpp).
bool is_valid_token_text(std::string_view text, std::size_t max_length) noexcept;
std::string render_hex(std::span<const std::uint8_t> bytes);
bool parse_hex_bytes(std::string_view hex, std::span<std::uint8_t> out) noexcept;

// Fixed-size binary identity (128-bit identifiers).
template <typename Tag, std::size_t ByteCount>
class FixedId {
 public:
  using tag_type = Tag;
  static constexpr std::size_t kByteCount = ByteCount;
  static constexpr std::string_view kName = Tag::kName;

  constexpr FixedId() noexcept = default;
  explicit constexpr FixedId(const std::array<std::uint8_t, ByteCount>& bytes) noexcept : bytes_(bytes) {}

  constexpr const std::array<std::uint8_t, ByteCount>& bytes() const noexcept { return bytes_; }

  constexpr bool is_zero() const noexcept {
    for (std::size_t i = 0; i < ByteCount; ++i) {
      if (bytes_[i] != 0) {
        return false;
      }
    }
    return true;
  }

  // A zero identifier is never a valid identity.
  constexpr bool is_valid() const noexcept { return !is_zero(); }

  std::string render() const { return render_hex(bytes_); }
  std::string to_hex() const { return render_hex(bytes_); }

  static Expected<FixedId> parse(std::string_view hex) {
    if (hex.size() != ByteCount * 2) {
      return make_error<FixedId>(StatusCode::MalformedEncoding,
                                 std::string(kName) + " must be exactly " + to_decimal(ByteCount * 2) +
                                     " hexadecimal characters");
    }
    std::array<std::uint8_t, ByteCount> bytes{};
    if (!parse_hex_bytes(hex, bytes)) {
      return make_error<FixedId>(StatusCode::MalformedEncoding,
                                 std::string(kName) + " contains a non-hexadecimal character");
    }
    const FixedId id(bytes);
    if (!id.is_valid()) {
      return make_error<FixedId>(StatusCode::MalformedEncoding, std::string(kName) + " must not be zero");
    }
    return id;
  }

  void write(ByteWriter& writer) const { writer.raw(bytes_); }

  static bool read(ByteReader& reader, FixedId& out) {
    std::span<const std::uint8_t> raw;
    if (!reader.raw(ByteCount, raw)) {
      return false;
    }
    std::array<std::uint8_t, ByteCount> bytes{};
    for (std::size_t i = 0; i < ByteCount; ++i) {
      bytes[i] = raw[i];
    }
    out = FixedId(bytes);
    return out.is_valid();
  }

  static FixedId from_digest(const Digest128& digest) {
    static_assert(ByteCount == 16, "from_digest requires a 128-bit identifier");
    return FixedId(digest.bytes());
  }

  std::size_t hash() const noexcept {
    return static_cast<std::size_t>(mix64(read_le64(bytes_.data()) ^ read_le64(bytes_.data() + 8)));
  }

  friend bool operator==(const FixedId&, const FixedId&) = default;
  friend auto operator<=>(const FixedId&, const FixedId&) = default;

 private:
  std::array<std::uint8_t, ByteCount> bytes_{};
};

// Bounded, checked ASCII identity token. The accepted character set is
// [A-Za-z0-9._-] with an alphanumeric first character, which keeps every
// rendering shell- and log-safe.
template <typename Tag, std::size_t MaxLength>
class TokenId {
 public:
  using tag_type = Tag;
  static constexpr std::size_t kMaxLength = MaxLength;
  static constexpr std::string_view kName = Tag::kName;

  TokenId() = default;
  explicit TokenId(std::string value) : value_(std::move(value)) {}

  static Expected<TokenId> parse(std::string_view text) {
    if (!is_valid_token_text(text, MaxLength)) {
      return make_error<TokenId>(
          StatusCode::MalformedEncoding,
          std::string(kName) + " must be 1.." + to_decimal(MaxLength) +
              " characters from [A-Za-z0-9._-] starting with an alphanumeric character");
    }
    return TokenId(std::string(text));
  }

  const std::string& value() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }
  bool is_valid() const noexcept { return is_valid_token_text(value_, MaxLength); }
  std::string render() const { return value_; }
  std::string to_string() const { return value_; }

  void write(ByteWriter& writer) const { writer.string(value_); }

  static bool read(ByteReader& reader, TokenId& out) {
    std::string text;
    if (!reader.string(MaxLength, text)) {
      return false;
    }
    if (!is_valid_token_text(text, MaxLength)) {
      return false;
    }
    out = TokenId(std::move(text));
    return true;
  }

  std::size_t hash() const noexcept {
    Fnv1a64 hasher;
    hasher.update(std::string_view(value_));
    return static_cast<std::size_t>(mix64(hasher.value()));
  }

  friend bool operator==(const TokenId&, const TokenId&) = default;
  friend auto operator<=>(const TokenId&, const TokenId&) = default;

 private:
  std::string value_;
};

// Checked unsigned 64-bit counter identity. Zero is never a valid value;
// incrementing never wraps.
template <typename Tag>
class CounterId {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;
  static constexpr std::string_view kName = Tag::kName;

  constexpr CounterId() noexcept = default;
  static constexpr CounterId from_value(std::uint64_t value) noexcept { return CounterId(value); }

  static Expected<CounterId> parse(std::string_view text) {
    std::uint64_t value = 0;
    if (!parse_u64_decimal(text, kMaxValue, value) || value == 0) {
      return make_error<CounterId>(StatusCode::MalformedEncoding,
                                   std::string(kName) + " must be a decimal integer in [1, " +
                                       to_decimal(kMaxValue) + "]");
    }
    return CounterId(value);
  }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }
  constexpr bool is_valid() const noexcept { return value_ != 0; }
  std::string render() const { return to_decimal(value_); }
  std::string to_string() const { return to_decimal(value_); }

  // Checked monotonic increment: overflow is an explicit error, never a wrap.
  Expected<CounterId> next() const {
    if (value_ >= kMaxValue) {
      return make_error<CounterId>(StatusCode::StaleGeneration,
                                   std::string(kName) + " would overflow its checked range");
    }
    return CounterId(value_ + 1);
  }

  void write(ByteWriter& writer) const { writer.u64(value_); }

  static bool read(ByteReader& reader, CounterId& out) {
    std::uint64_t value = 0;
    if (!reader.u64(value)) {
      return false;
    }
    if (value == 0 || value > kMaxValue) {
      return false;
    }
    out = CounterId(value);
    return true;
  }

  std::size_t hash() const noexcept { return static_cast<std::size_t>(mix64(value_)); }

  friend bool operator==(const CounterId&, const CounterId&) = default;
  friend auto operator<=>(const CounterId&, const CounterId&) = default;

  // Highest representable value; leaving headroom keeps "one past the end"
  // representable for watermarks.
  static constexpr std::uint64_t kMaxValue = 0xFFFFFFFFFFFFFFFEull;

 private:
  explicit constexpr CounterId(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Concrete identities
// ---------------------------------------------------------------------------

struct RouteIdTag {
  static constexpr std::string_view kName = "RouteId";
};
struct RouteSnapshotIdTag {
  static constexpr std::string_view kName = "RouteSnapshotId";
};
struct MutationAttemptIdTag {
  static constexpr std::string_view kName = "MutationAttemptId";
};
struct ProgrammingAttemptIdTag {
  static constexpr std::string_view kName = "ProgrammingAttemptId";
};
struct WorkerBootIdTag {
  static constexpr std::string_view kName = "WorkerBootId";
};
struct PathIdTag {
  static constexpr std::string_view kName = "PathId";
};
struct NextHopIdTag {
  static constexpr std::string_view kName = "NextHopId";
};
struct NextHopGroupIdTag {
  static constexpr std::string_view kName = "NextHopGroupId";
};
struct DestinationIdTag {
  static constexpr std::string_view kName = "DestinationId";
};

struct PublisherIdTag {
  static constexpr std::string_view kName = "PublisherId";
};
struct BackendIdTag {
  static constexpr std::string_view kName = "BackendId";
};
struct FabricIdTag {
  static constexpr std::string_view kName = "FabricId";
};
struct RoutingNamespaceTag {
  static constexpr std::string_view kName = "RoutingNamespace";
};

struct RouteGenerationTag {
  static constexpr std::string_view kName = "RouteGeneration";
};
struct RouteAuthorityGenerationTag {
  static constexpr std::string_view kName = "RouteAuthorityGeneration";
};
struct CoordinatorEpochTag {
  static constexpr std::string_view kName = "CoordinatorEpoch";
};
struct PathAuthorityGenerationTag {
  static constexpr std::string_view kName = "PathAuthorityGeneration";
};
struct PolicyGenerationTag {
  static constexpr std::string_view kName = "PolicyGeneration";
};
struct ProgrammingGenerationTag {
  static constexpr std::string_view kName = "ProgrammingGeneration";
};

using RouteId = FixedId<RouteIdTag, 16>;
using RouteSnapshotId = FixedId<RouteSnapshotIdTag, 16>;
using MutationAttemptId = FixedId<MutationAttemptIdTag, 16>;
using ProgrammingAttemptId = FixedId<ProgrammingAttemptIdTag, 16>;
using WorkerBootId = FixedId<WorkerBootIdTag, 16>;
using PathId = FixedId<PathIdTag, 16>;
using NextHopId = FixedId<NextHopIdTag, 16>;
using NextHopGroupId = FixedId<NextHopGroupIdTag, 16>;
using DestinationId = FixedId<DestinationIdTag, 16>;

using PublisherId = TokenId<PublisherIdTag, 64>;
using BackendId = TokenId<BackendIdTag, 64>;
using FabricId = TokenId<FabricIdTag, 64>;
using RoutingNamespace = TokenId<RoutingNamespaceTag, 64>;

using RouteGeneration = CounterId<RouteGenerationTag>;
using RouteAuthorityGeneration = CounterId<RouteAuthorityGenerationTag>;
using CoordinatorEpoch = CounterId<CoordinatorEpochTag>;
using PathAuthorityGeneration = CounterId<PathAuthorityGenerationTag>;
using PolicyGeneration = CounterId<PolicyGenerationTag>;
using ProgrammingGeneration = CounterId<ProgrammingGenerationTag>;

// Hash functor usable as the second template argument of the unordered
// containers used for the runtime indexes.
struct IdHash {
  template <typename Id>
  std::size_t operator()(const Id& id) const noexcept {
    return id.hash();
  }
};

// Generates never-zero 128-bit identities from a seeded source.
template <typename Id>
Id generate_id(SeededIdSource& source) {
  static_assert(Id::kByteCount == 16, "generate_id requires a 128-bit identifier");
  for (;;) {
    const Id candidate = Id::from_digest(source.next_digest());
    if (candidate.is_valid()) {
      return candidate;
    }
  }
}

}  // namespace routefabric

#endif  // ROUTEFABRIC_IDS_HPP
