// Shared helpers for the Route Fabric executables.
#ifndef ROUTEFABRIC_TOOLS_COMMON_HPP
#define ROUTEFABRIC_TOOLS_COMMON_HPP

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "routefabric/destination.hpp"
#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"
#include "routefabric/record.hpp"
#include "routefabric/route_key.hpp"
#include "routefabric/version.hpp"

namespace routefabric {
namespace tools {

inline void print_line(const std::string& text) {
  std::cout << text << '\n';
}

inline void print_error(const Error& error) {
  std::cout << "error " << to_string(error.code()) << ": " << error.detail() << '\n';
  std::cout.flush();
}

inline void flush_output() { std::cout.flush(); }

// Atomically replaces the contents of a status file so that a polling reader
// never observes a partially written file.
inline bool write_status_file(const std::filesystem::path& path, const std::string& contents) {
  const std::filesystem::path temporary(path.string() + ".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << contents;
    out.flush();
    if (!out) {
      return false;
    }
  }
#ifdef _WIN32
  // A concurrent reader may hold the destination open for a moment; replacing it
  // is retried a bounded number of times instead of failing the status update.
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (::MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
#else
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
#endif
}

inline std::string hex_of(const std::uint8_t* bytes, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = kDigits[(bytes[i] >> 4) & 0x0Fu];
    out[(2 * i) + 1] = kDigits[bytes[i] & 0x0Fu];
  }
  return out;
}

template <typename Id>
inline std::string render_id(const Id& id) {
  return id.is_valid() ? id.render() : std::string("-");
}

inline std::string parse_hex_seed(std::string_view text) {
  return std::string(text);
}

}  // namespace tools
}  // namespace routefabric

#endif  // ROUTEFABRIC_TOOLS_COMMON_HPP
