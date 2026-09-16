#include "routefabric/hash.hpp"

#include <cstdio>
#include <random>

namespace routefabric {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli polynomial
  // The table is filled through a range loop so no index arithmetic is needed.
  std::uint32_t index = 0;
  for (std::uint32_t& slot : table) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPoly : (crc >> 1);
    }
    slot = crc;
    ++index;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr std::uint64_t kFnvPrime = 0x00000100000001B3ull;
constexpr std::uint64_t kFnvBasisAlt = 0x9AE16A3B2F90404Full;

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

void Crc32c::update(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = state_;
  for (const std::uint8_t byte : data) {
    crc = kCrc32cTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  state_ = crc;
}

void Crc32c::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void Crc32c::update_byte(std::uint8_t value) noexcept {
  state_ = kCrc32cTable[(state_ ^ value) & 0xFFu] ^ (state_ >> 8);
}

std::uint32_t Crc32c::compute(std::span<const std::uint8_t> data) noexcept {
  Crc32c crc;
  crc.update(data);
  return crc.value();
}

void Fnv1a64::update(std::span<const std::uint8_t> data) noexcept {
  std::uint64_t hash = state_;
  for (const std::uint8_t byte : data) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnvPrime;
  }
  state_ = hash;
}

void Fnv1a64::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void Fnv1a64::update_byte(std::uint8_t value) noexcept {
  state_ ^= static_cast<std::uint64_t>(value);
  state_ *= kFnvPrime;
}

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

Digest128 Digest128::from_u64_pair(std::uint64_t high, std::uint64_t low) noexcept {
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>((high >> (56 - (8 * i))) & 0xFFu);
    bytes[8 + i] = static_cast<std::uint8_t>((low >> (56 - (8 * i))) & 0xFFu);
  }
  return Digest128(bytes);
}

bool Digest128::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest128::to_hex() const {
  std::string out;
  out.resize(32);
  for (std::size_t i = 0; i < 16; ++i) {
    out[2 * i] = kHexDigits[(bytes_[i] >> 4) & 0x0Fu];
    out[(2 * i) + 1] = kHexDigits[bytes_[i] & 0x0Fu];
  }
  return out;
}

Expected<Digest128> Digest128::parse(std::string_view hex) {
  if (hex.size() != 32) {
    return make_error<Digest128>(StatusCode::MalformedEncoding,
                                 "digest must be exactly 32 hexadecimal characters");
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    const int hi = hex_value(hex[2 * i]);
    const int lo = hex_value(hex[(2 * i) + 1]);
    if (hi < 0 || lo < 0) {
      return make_error<Digest128>(StatusCode::MalformedEncoding, "digest contains a non-hexadecimal character");
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  if (Digest128(bytes).is_zero()) {
    return make_error<Digest128>(StatusCode::MalformedEncoding, "digest must not be zero");
  }
  return Digest128(bytes);
}

Digest128 digest128(std::string_view domain, std::span<const std::uint8_t> data) {
  // Two structurally different lanes: a forward FNV-1a pass and a reverse FNV-1a
  // pass with an alternate basis. Both lanes depend on every payload byte, and
  // neither is an affine function of the other, so the digest carries the full
  // 128 bits of state rather than one 64-bit value and a derived image.
  Fnv1a64 forward(0xCBF29CE484222325ull);
  Fnv1a64 backward(kFnvBasisAlt);
  const auto domain_bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(domain.data()), domain.size());
  forward.update(domain_bytes);
  backward.update(domain_bytes);
  const std::uint8_t separator = 0;
  forward.update_byte(separator);
  backward.update_byte(separator);
  forward.update(data);
  for (std::size_t i = data.size(); i-- > 0;) {
    backward.update_byte(data[i]);
  }
  const std::uint64_t length = static_cast<std::uint64_t>(data.size());
  const std::uint64_t high = mix64(forward.value() ^ mix64(length));
  const std::uint64_t low = mix64(backward.value() ^ (length * 0x9E3779B97F4A7C15ull));
  return Digest128::from_u64_pair(high, low);
}

Digest128 digest128(std::string_view domain, std::string_view data) {
  return digest128(domain, std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

SeededIdSource::SeededIdSource(std::uint64_t seed) noexcept {
  state_[0] = mix64(seed);
  state_[1] = mix64(state_[0]);
  state_[2] = mix64(state_[1]);
  state_[3] = mix64(state_[2]);
  if (state_[0] == 0 && state_[1] == 0 && state_[2] == 0 && state_[3] == 0) {
    state_[0] = 0x9E3779B97F4A7C15ull;
  }
}

std::uint64_t SeededIdSource::next_u64() noexcept {
  // xoshiro256** step.
  const std::uint64_t result = ((state_[1] * 5ull) << 7) | ((state_[1] * 5ull) >> 57);
  const std::uint64_t t = state_[1] << 17;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = (state_[3] << 45) | (state_[3] >> 19);
  return result * 9ull;
}

Digest128 SeededIdSource::next_digest() noexcept {
  const std::uint64_t high = next_u64();
  const std::uint64_t low = next_u64();
  return Digest128::from_u64_pair(high, low);
}

std::uint64_t SeededIdSource::system_seed() noexcept {
  std::random_device device;
  const std::uint64_t a = static_cast<std::uint64_t>(device());
  const std::uint64_t b = static_cast<std::uint64_t>(device());
  return mix64((a << 32) ^ b);
}

}  // namespace routefabric
