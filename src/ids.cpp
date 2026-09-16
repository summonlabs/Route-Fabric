#include "routefabric/ids.hpp"

namespace routefabric {
namespace {

constexpr bool is_token_char(char c) noexcept {
  const bool digit = c >= '0' && c <= '9';
  const bool lower = c >= 'a' && c <= 'z';
  const bool upper = c >= 'A' && c <= 'Z';
  return digit || lower || upper || c == '.' || c == '_' || c == '-';
}

constexpr bool is_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

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

bool is_valid_token_text(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  if (!is_alphanumeric(text.front())) {
    return false;
  }
  for (const char c : text) {
    if (!is_token_char(c)) {
      return false;
    }
  }
  return true;
}

std::string render_hex(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kHexDigits[(bytes[i] >> 4) & 0x0Fu];
    out[(2 * i) + 1] = kHexDigits[bytes[i] & 0x0Fu];
  }
  return out;
}

bool parse_hex_bytes(std::string_view hex, std::span<std::uint8_t> out) noexcept {
  if (hex.size() != out.size() * 2) {
    return false;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = hex_value(hex[2 * i]);
    const int lo = hex_value(hex[(2 * i) + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace routefabric
