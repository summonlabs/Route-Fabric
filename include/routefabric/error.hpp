#ifndef ROUTEFABRIC_ERROR_HPP
#define ROUTEFABRIC_ERROR_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace routefabric {

// Stable status codes. The numeric values are part of the wire protocol and the
// persistence format: they must never change once released.
enum class StatusCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  MalformedEncoding = 2,
  NotFound = 3,
  AlreadyExists = 4,
  Conflict = 5,
  StaleEpoch = 6,
  StaleWorkerBoot = 7,
  StaleGeneration = 8,
  Unauthorized = 9,
  ScopeViolation = 10,
  PathAuthorityRejected = 11,
  PathAuthorityStale = 12,
  LifecycleViolation = 13,
  Retired = 14,
  Revoked = 15,
  BackendUnavailable = 16,
  BackendFailure = 17,
  AmbiguousOutcome = 18,
  LimitExceeded = 19,
  PersistenceFailure = 20,
  ProtocolViolation = 21,
  IntegrityFailure = 22,
  Unsupported = 23,
  ReentrancyViolation = 24,
  Internal = 25,
  NotRegistered = 26,
  Fenced = 27,
  ShuttingDown = 28,
  NotOpen = 29,
};

// Stable, script-friendly rendering of a status code.
const char* to_string(StatusCode code) noexcept;

// True when the code denotes a successful operation.
constexpr bool is_ok(StatusCode code) noexcept { return code == StatusCode::Ok; }

class Error {
 public:
  Error() = default;
  Error(StatusCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  StatusCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }

  // "<code>: <detail>" (detail omitted when empty).
  std::string render() const;

  friend bool operator==(const Error& a, const Error& b) {
    return a.code_ == b.code_ && a.detail_ == b.detail_;
  }

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string detail_;
};

// Result type used across the public API. Errors are values, not exceptions.
template <class T>
class [[nodiscard]] Expected {
 public:
  Expected(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Expected(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  const T& value() const { return std::get<0>(storage_); }
  T& value() { return std::get<0>(storage_); }
  T&& take() { return std::move(std::get<0>(storage_)); }

  const Error& error() const { return std::get<1>(storage_); }
  StatusCode code() const noexcept { return has_value() ? StatusCode::Ok : error().code(); }

 private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Expected<void> {
 public:
  Expected() = default;
  Expected(Error error) : error_(std::move(error)), ok_(false) {}

  bool has_value() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  void value() const noexcept {}
  const Error& error() const { return error_; }
  StatusCode code() const noexcept { return ok_ ? StatusCode::Ok : error_.code(); }

 private:
  Error error_;
  bool ok_ = true;
};

using Status = Expected<void>;

inline Status ok_status() { return Status(); }

inline Status make_error(StatusCode code, std::string detail) {
  return Status(Error(code, std::move(detail)));
}

template <class T>
inline Expected<T> make_error(StatusCode code, std::string detail) {
  return Expected<T>(Error(code, std::move(detail)));
}

}  // namespace routefabric

#endif  // ROUTEFABRIC_ERROR_HPP
