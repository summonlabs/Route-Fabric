#ifndef ROUTEFABRIC_HASH_HPP
#define ROUTEFABRIC_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "routefabric/error.hpp"

namespace routefabric {

// CRC-32C (Castagnoli). Used for integrity trailers and frame integrity.
class Crc32c {
 public:
  Crc32c() = default;

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFu; }
  void reset() noexcept { state_ = 0xFFFFFFFFu; }

  static std::uint32_t compute(std::span<const std::uint8_t> data) noexcept;

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

// FNV-1a 64 with a configurable basis, used for content digests.
class Fnv1a64 {
 public:
  explicit Fnv1a64(std::uint64_t basis = 0xCBF29CE484222325ull) noexcept : state_(basis) {}

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

// 128-bit content digest. This is a deterministic, non-cryptographic digest: it
// establishes content identity and detects accidental corruption. It is not a
// MAC and provides no authentication.
class Digest128 {
 public:
  Digest128() = default;
  explicit Digest128(std::array<std::uint8_t, 16> bytes) : bytes_(bytes) {}

  static Digest128 from_u64_pair(std::uint64_t high, std::uint64_t low) noexcept;

  const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }
  bool is_zero() const noexcept;
  std::string to_hex() const;
  static Expected<Digest128> parse(std::string_view hex);

  friend bool operator==(const Digest128&, const Digest128&) = default;
  friend auto operator<=>(const Digest128&, const Digest128&) = default;

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

// Domain-separated digest: digest(domain || 0x00 || data).
Digest128 digest128(std::string_view domain, std::span<const std::uint8_t> data);
Digest128 digest128(std::string_view domain, std::string_view data);

// Deterministic pseudo-random generator with an explicit seed. Identifiers that
// must be reproducible under test are produced from a seeded generator.
class SeededIdSource {
 public:
  explicit SeededIdSource(std::uint64_t seed) noexcept;

  std::uint64_t next_u64() noexcept;
  Digest128 next_digest() noexcept;

  // A non-deterministic seed derived from the platform entropy source.
  static std::uint64_t system_seed() noexcept;

 private:
  std::uint64_t state_[4];
};

// Mixing helper (splitmix64 finalizer).
std::uint64_t mix64(std::uint64_t value) noexcept;

}  // namespace routefabric

#endif  // ROUTEFABRIC_HASH_HPP
