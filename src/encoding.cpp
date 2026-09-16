#include "routefabric/encoding.hpp"

#include <limits>

namespace routefabric {

void ByteWriter::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::raw(std::span<const std::uint8_t> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void ByteWriter::raw(std::string_view data) {
  raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void ByteWriter::string(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value);
}

bool ByteReader::u8(std::uint8_t& out) {
  if (remaining() < 1) {
    return false;
  }
  out = data_[offset_];
  offset_ += 1;
  return true;
}

bool ByteReader::boolean(bool& out) {
  std::uint8_t value = 0;
  if (!u8(value)) {
    return false;
  }
  if (value > 1) {
    return false;
  }
  out = value == 1;
  return true;
}

bool ByteReader::u16(std::uint16_t& out) {
  if (remaining() < 2) {
    return false;
  }
  out = read_le16(data_.data() + offset_);
  offset_ += 2;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) {
  if (remaining() < 4) {
    return false;
  }
  out = read_le32(data_.data() + offset_);
  offset_ += 4;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) {
  if (remaining() < 8) {
    return false;
  }
  out = read_le64(data_.data() + offset_);
  offset_ += 8;
  return true;
}

bool ByteReader::raw(std::size_t count, std::span<const std::uint8_t>& out) {
  if (remaining() < count) {
    return false;
  }
  out = data_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool ByteReader::string(std::size_t max_length, std::string& out) {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (static_cast<std::size_t>(length) > max_length) {
    return false;
  }
  if (remaining() < static_cast<std::size_t>(length)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(data_.data() + offset_), static_cast<std::size_t>(length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool ByteReader::skip(std::size_t count) {
  if (remaining() < count) {
    return false;
  }
  offset_ += count;
  return true;
}

std::uint16_t read_le16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t read_le32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data[i]) << (8 * i);
  }
  return value;
}

std::uint64_t read_le64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }
  return value;
}

void write_le16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void write_le32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void write_le64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

bool parse_u64_decimal(std::string_view text, std::uint64_t max_value, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  if (text.size() > 1 && text.front() == '0') {
    return false;  // reject leading zeros so decimal rendering is canonical
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ull) {
      return false;
    }
    value = (value * 10ull) + digit;
  }
  if (value > max_value) {
    return false;
  }
  out = value;
  return true;
}

std::string to_decimal(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  char buffer[24] = {};
  std::size_t index = sizeof(buffer);
  while (value != 0 && index > 0) {
    --index;
    buffer[index] = static_cast<char>('0' + (value % 10ull));
    value /= 10ull;
  }
  return std::string(buffer + index, sizeof(buffer) - index);
}

}  // namespace routefabric
