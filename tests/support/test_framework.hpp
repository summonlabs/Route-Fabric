// Minimal deterministic test framework for Route Fabric.
//
// Design constraints:
//   * no framework timeouts: a hanging test is a defect, not something to hide;
//   * bounded internal waits fail with an explicit assertion;
//   * tests run to completion and report every recorded failure.
#ifndef ROUTEFABRIC_TEST_FRAMEWORK_HPP
#define ROUTEFABRIC_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <filesystem>

#include "routefabric/error.hpp"
#include <functional>
#include <string>
#include <vector>

namespace rftest {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

std::vector<TestCase>& registry();
bool register_test(const char* name, TestFunction function);

// Records a failure and returns false when the expectation does not hold.
bool check(bool condition, const char* expression, const char* file, int line);
void fail(const std::string& message, const char* file, int line);

// Runs every registered test whose name contains the filter (empty runs all).
int run_all(const std::string& filter);

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

// Creates a unique directory under the process temporary root. Uniqueness is
// process- and counter-based, never time-based.
std::filesystem::path make_temp_directory(const std::string& tag);
void remove_directory(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------

// A real operating-system child process with redirected standard handles.
// Standard output and error go to files, which avoids pipe deadlocks and keeps
// child stdin from controlling process lifetime.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool Start(const std::vector<std::string>& arguments, const std::string& executable, const std::string& log_path);
  bool running();
  // Hard termination through the operating system termination path.
  bool Terminate();
  bool WaitForExit(std::uint32_t& exit_code);
  std::uint32_t process_id() const { return process_id_; }

 private:
  void* process_handle_ = nullptr;
  std::uint32_t process_id_ = 0;
};

// Resolves a sibling executable next to the running test binary.
std::string executable_path(const std::string& name);

// Polls for a status line in a file. Returns false and fills 'why' when the
// bounded number of attempts is exhausted: that is an explicit test failure, not
// a silent wait.
bool wait_for_file_line(const std::filesystem::path& path, const std::string& prefix, int max_attempts,
                        std::string& value, std::string& why);

bool wait_for_file_exists(const std::filesystem::path& path, int max_attempts, std::string& why);

// Reads a whole text file; returns false when it cannot be read.
bool read_text_file(const std::filesystem::path& path, std::string& contents);

// Extracts "key value" from a line-oriented text blob.
bool find_line_value(const std::string& text, const std::string& key, std::string& value);

// ---------------------------------------------------------------------------
// Deterministic scheduling helpers
// ---------------------------------------------------------------------------

// A one-shot latch used to force orderings between threads.
class Latch {
 public:
  void Release();
  // Bounded wait: exhaustion is an explicit failure of the waiting test.
  bool Wait(int max_attempts);
  bool released() const { return released_; }

 private:
  volatile bool released_ = false;
};

}  // namespace rftest

#define RF_TEST(name)                                                     \
  static void name();                                                     \
  static const bool rf_registered_##name = ::rftest::register_test(#name, &name); \
  static void name()

#define RF_CHECK(expression) \
  (void)(::rftest::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__))

#define RF_CHECK_EQ(actual, expected)                                                          \
  do {                                                                                         \
    const auto rf_actual = (actual);                                                           \
    const auto rf_expected = (expected);                                                       \
    (void)::rftest::check(rf_actual == rf_expected, #actual " == " #expected, __FILE__, __LINE__); \
  } while (false)

#define RF_REQUIRE(expression)                                                       \
  do {                                                                               \
    if (!::rftest::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)) { \
      return;                                                                        \
    }                                                                                \
  } while (false)

#define RF_REQUIRE_EQ(actual, expected)                                                          do {                                                                                             const auto rf_actual = (actual);                                                               const auto rf_expected = (expected);                                                           if (!::rftest::check(rf_actual == rf_expected, #actual " == " #expected, __FILE__, __LINE__)) {       return;                                                                                      }                                                                                            } while (false)

// Requires an Expected<T> to hold a value and reports the structured error when
// it does not.
#define RF_REQUIRE_OK(expression)                                                                do {                                                                                             auto rf_result = (expression);                                                                 if (!::rftest::check(static_cast<bool>(rf_result), #expression, __FILE__, __LINE__)) {            ::rftest::fail(std::string("error: ") + ::routefabric::to_string(rf_result.error().code()) + \
                         " " + rf_result.error().detail(),                                                          __FILE__, __LINE__);                                                            return;                                                                                      }                                                                                            } while (false)

#define RF_FAIL(message) ::rftest::fail((message), __FILE__, __LINE__)

#endif  // ROUTEFABRIC_TEST_FRAMEWORK_HPP
