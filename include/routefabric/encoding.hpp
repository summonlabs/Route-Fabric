#ifndef ROUTEFABRIC_ENCODING_HPP
#define ROUTEFABRIC_ENCODING_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace routefabric {

// Deterministic little-endian byte writer used by both the persistence format
// and the wire codec. No C++ object layout is ever serialized.
class ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(std::size_t reserve_bytes) { buffer_.reserve(reserve_bytes); }

  void u8(std::uint8_t value) { buffer_.push_back(value); }
  void boolean(bool value) { buffer_.push_back(value ? 1u : 0u); }
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void raw(std::span<const std::uint8_t> data);
  void raw(std::string_view data);
  // Length-prefixed (u32) byte string.
  void string(std::string_view value);

  const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  std::vector<std::uint8_t> take() { return std::move(buffer_); }
  std::size_t size() const noexcept { return buffer_.size(); }
  bool empty() const noexcept { return buffer_.empty(); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::vector<std::uint8_t> buffer_;
};

// Strict bounds-checked little-endian reader. Every accessor returns false on
// underflow or on a violated bound; no accessor throws.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

  bool u8(std::uint8_t& out);
  bool boolean(bool& out);
  bool u16(std::uint16_t& out);
  bool u32(std::uint32_t& out);
  bool u64(std::uint64_t& out);
  bool raw(std::size_t count, std::span<const std::uint8_t>& out);
  bool string(std::size_t max_length, std::string& out);
  bool skip(std::size_t count);

  bool at_end() const noexcept { return offset_ == data_.size(); }
  std::size_t remaining() const noexcept { return data_.size() - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  std::span<const std::uint8_t> rest() const { return data_.subspan(offset_); }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
};

// Machine-independent byte order helpers.
std::uint16_t read_le16(const std::uint8_t* data) noexcept;
std::uint32_t read_le32(const std::uint8_t* data) noexcept;
std::uint64_t read_le64(const std::uint8_t* data) noexcept;
void write_le16(std::uint8_t* out, std::uint16_t value) noexcept;
void write_le32(std::uint8_t* out, std::uint32_t value) noexcept;
void write_le64(std::uint8_t* out, std::uint64_t value) noexcept;

// Parses an unsigned decimal integer with strict validation: no sign, no
// leading zeros (except "0"), no whitespace, bounded length, overflow checked.
bool parse_u64_decimal(std::string_view text, std::uint64_t max_value, std::uint64_t& out) noexcept;

std::string to_decimal(std::uint64_t value);

}  // namespace routefabric

#endif  // ROUTEFABRIC_ENCODING_HPP
