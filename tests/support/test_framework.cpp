#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rftest {
namespace {

std::string& current_test_name() {
  static std::string name;
  return name;
}

int& current_test_failures() {
  static int failures = 0;
  return failures;
}

std::atomic<std::uint64_t> g_temp_counter{0};

#ifdef _WIN32
// Every child process is assigned to a job object that terminates its members
// when the job handle closes, so a child can never outlive the test process even
// if a test aborts before its cleanup.
HANDLE process_job() {
  static HANDLE job = []() -> HANDLE {
    HANDLE created = ::CreateJobObjectW(nullptr, nullptr);
    if (created == nullptr) {
      return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (::SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
      ::CloseHandle(created);
      return nullptr;
    }
    return created;
  }();
  return job;
}
#endif

std::filesystem::path process_temp_root() {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
  if (length > 0) {
    return std::filesystem::path(buffer);
  }
  return std::filesystem::path("C:/Windows/Temp");
#else
  const char* value = std::getenv("TMPDIR");
  return value == nullptr ? std::filesystem::path("/tmp") : std::filesystem::path(value);
#endif
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

bool register_test(const char* name, TestFunction function) {
  registry().push_back(TestCase{name, function});
  return true;
}

bool check(bool condition, const char* expression, const char* file, int line) {
  if (condition) {
    return true;
  }
  ++current_test_failures();
  std::cout << "  failure: " << file << ":" << line << ": " << expression << '\n';
  return false;
}

void fail(const std::string& message, const char* file, int line) {
  ++current_test_failures();
  std::cout << "  failure: " << file << ":" << line << ": " << message << '\n';
}

int run_all(const std::string& filter) {
  std::vector<TestCase> tests = registry();
  std::sort(tests.begin(), tests.end(),
            [](const TestCase& a, const TestCase& b) { return a.name < b.name; });
  int failed_tests = 0;
  int executed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const TestCase& test : tests) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    current_test_name() = test.name;
    current_test_failures() = 0;
    std::cout << "RUN " << test.name << '\n';
    std::cout.flush();
    const auto test_start = std::chrono::steady_clock::now();
    test.function();
    const auto test_end = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start).count();
    if (current_test_failures() == 0) {
      std::cout << "PASS " << test.name << " (" << elapsed << " ms)\n";
    } else {
      std::cout << "FAIL " << test.name << " (" << current_test_failures() << " failures, " << elapsed << " ms)\n";
      ++failed_tests;
    }
    std::cout.flush();
  }
  const auto end = std::chrono::steady_clock::now();
  const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "tests " << executed << " failed " << failed_tests << " elapsed-ms " << total << '\n';
  return failed_tests == 0 ? 0 : 1;
}

std::filesystem::path make_temp_directory(const std::string& tag) {
  const std::uint64_t counter = g_temp_counter.fetch_add(1);
#ifdef _WIN32
  const std::uint32_t pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  const std::uint32_t pid = static_cast<std::uint32_t>(::getpid());
#endif
  std::ostringstream name;
  name << "routefabric-test-" << tag << "-" << pid << "-" << counter;
  const std::filesystem::path path = process_temp_root() / name.str();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path;
}

void remove_directory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

ChildProcess::~ChildProcess() {
#ifdef _WIN32
  if (process_handle_ != nullptr) {
    HANDLE handle = static_cast<HANDLE>(process_handle_);
    // A child must never outlive the test that started it.
    DWORD code = 0;
    if (::GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE) {
      (void)::TerminateProcess(handle, 1);
      (void)::WaitForSingleObject(handle, INFINITE);
    }
    ::CloseHandle(handle);
    process_handle_ = nullptr;
  }
#endif
}

bool ChildProcess::Start(const std::vector<std::string>& arguments, const std::string& executable,
                         const std::string& log_path) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE log = ::CreateFileW(std::wstring(log_path.begin(), log_path.end()).c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) {
    return false;
  }
  HANDLE null_input = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::string command_line = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command_line += " \"" + argument + "\"";
  }
  std::wstring wide_command(command_line.begin(), command_line.end());
  std::wstring wide_executable(executable.begin(), executable.end());

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = log;
  startup.hStdError = log;
  startup.hStdInput = null_input == INVALID_HANDLE_VALUE ? nullptr : null_input;

  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(wide_executable.c_str(), wide_command.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
  ::CloseHandle(log);
  if (null_input != INVALID_HANDLE_VALUE) {
    ::CloseHandle(null_input);
  }
  if (created == 0) {
    return false;
  }
  ::CloseHandle(process.hThread);
  if (HANDLE job = process_job(); job != nullptr) {
    (void)::AssignProcessToJobObject(job, process.hProcess);
  }
  process_handle_ = process.hProcess;
  process_id_ = process.dwProcessId;
  return true;
#else
  (void)arguments;
  (void)executable;
  (void)log_path;
  return false;
#endif
}

bool ChildProcess::running() {
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return false;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) == 0) {
    return false;
  }
  return code == STILL_ACTIVE;
#else
  return false;
#endif
}

bool ChildProcess::Terminate() {
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return false;
  }
  const BOOL terminated = ::TerminateProcess(static_cast<HANDLE>(process_handle_), 1);
  if (terminated == 0) {
    return false;
  }
  return ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE) == WAIT_OBJECT_0;
#else
  return false;
#endif
}

bool ChildProcess::WaitForExit(std::uint32_t& exit_code) {
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return false;
  }
  if (::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE) != WAIT_OBJECT_0) {
    return false;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) == 0) {
    return false;
  }
  exit_code = static_cast<std::uint32_t>(code);
  return true;
#else
  (void)exit_code;
  return false;
#endif
}

std::string executable_path(const std::string& name) {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  const std::filesystem::path self(std::wstring(buffer, buffer + length));
  std::filesystem::path directory = self.parent_path();
  // Multi-config generators place the test binary one level below the tools.
  for (int level = 0; level < 3; ++level) {
    const std::filesystem::path candidate = directory / (name + ".exe");
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      return candidate.string();
    }
    if (!directory.has_parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return (self.parent_path() / (name + ".exe")).string();
#else
  return name;
#endif
}

bool wait_for_file_line(const std::filesystem::path& path, const std::string& prefix, int max_attempts,
                        std::string& value, std::string& why) {
  std::string contents;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (read_text_file(path, contents) && find_line_value(contents, prefix, value)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  why = "timed out waiting for '" + prefix + "' in " + path.string();
  return false;
}

bool wait_for_file_exists(const std::filesystem::path& path, int max_attempts, std::string& why) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  why = "timed out waiting for " + path.string() + " to appear";
  return false;
}

bool read_text_file(const std::filesystem::path& path, std::string& contents) {
#ifdef _WIN32
  // FILE_SHARE_DELETE is required so that a writer can atomically replace the
  // file while a reader has it open.
  HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0 || size.QuadPart < 0) {
    ::CloseHandle(handle);
    return false;
  }
  contents.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  if (!contents.empty()) {
    if (::ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) == 0) {
      ::CloseHandle(handle);
      return false;
    }
  }
  contents.resize(static_cast<std::size_t>(read));
  ::CloseHandle(handle);
  return true;
#else
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  contents = buffer.str();
  return true;
#endif
}

bool find_line_value(const std::string& text, const std::string& key, std::string& value) {
  std::istringstream stream(text);
  std::string line;
  const std::string prefix = key + "=";
  const std::string spaced = key + " ";
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line == key) {
      value.clear();
      return true;
    }
    if (line.rfind(prefix, 0) == 0) {
      value = line.substr(prefix.size());
      return true;
    }
    if (line.rfind(spaced, 0) == 0) {
      value = line.substr(spaced.size());
      return true;
    }
  }
  return false;
}

void Latch::Release() { released_ = true; }

bool Latch::Wait(int max_attempts) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (released_) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return released_;
}

}  // namespace rftest

int main(int argc, char** argv) {
  std::string filter;
  if (argc > 1) {
    filter = argv[1];
  }
  return rftest::run_all(filter);
}
