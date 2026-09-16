#include "routefabric/backend.hpp"

namespace routefabric {
namespace {
template <typename Enum>
bool parse_by_name(std::string_view text, Enum first, Enum last, const char* (*render)(Enum) noexcept, Enum& out) {
  for (std::uint8_t raw = static_cast<std::uint8_t>(first); raw <= static_cast<std::uint8_t>(last); ++raw) {
    const auto candidate = static_cast<Enum>(raw);
    if (text == render(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}
}  // namespace

const char* to_string(ProgrammingOutcome outcome) noexcept {
  switch (outcome) {
    case ProgrammingOutcome::Applied:
      return "APPLIED";
    case ProgrammingOutcome::Idempotent:
      return "IDEMPOTENT";
    case ProgrammingOutcome::NotSupported:
      return "NOT_SUPPORTED";
    case ProgrammingOutcome::Rejected:
      return "REJECTED";
    case ProgrammingOutcome::RetryableFailure:
      return "RETRYABLE_FAILURE";
    case ProgrammingOutcome::PermanentFailure:
      return "PERMANENT_FAILURE";
    case ProgrammingOutcome::Ambiguous:
      return "AMBIGUOUS";
    case ProgrammingOutcome::BackendUnavailable:
      return "BACKEND_UNAVAILABLE";
  }
  return "UNKNOWN";
}

bool parse_programming_outcome(std::string_view text, ProgrammingOutcome& out) noexcept {
  return parse_by_name(text, ProgrammingOutcome::Applied, ProgrammingOutcome::BackendUnavailable, to_string, out);
}

const char* to_string(ProgrammingOperation operation) noexcept {
  switch (operation) {
    case ProgrammingOperation::Install:
      return "install";
    case ProgrammingOperation::Replace:
      return "replace";
    case ProgrammingOperation::Withdraw:
      return "withdraw";
  }
  return "unknown";
}

const char* to_string(BackendPresence presence) noexcept {
  switch (presence) {
    case BackendPresence::Unknown:
      return "UNKNOWN";
    case BackendPresence::Present:
      return "PRESENT";
    case BackendPresence::Absent:
      return "ABSENT";
    case BackendPresence::Unavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

std::string RouteProgrammingRequest::render() const {
  std::string out = to_string(operation);
  out += " attempt=";
  out += attempt.render();
  out += " route=";
  out += route.render();
  out += " key=";
  out += key.render();
  out += " binding=";
  out += binding.render();
  out += " desired-gen=";
  out += desired_generation.render();
  out += " programming-gen=";
  out += programming_generation.render();
  return out;
}

SyntheticProgrammingBackend::SyntheticProgrammingBackend(BackendId id) : id_(std::move(id)) {}

BackendId SyntheticProgrammingBackend::id() const { return id_; }

BackendCapabilities SyntheticProgrammingBackend::capabilities() const {
  BackendCapabilities capabilities;
  capabilities.install = true;
  capabilities.replace = true;
  capabilities.withdraw = true;
  capabilities.query = true;
  capabilities.read_only = false;
  return capabilities;
}

void SyntheticProgrammingBackend::EnqueueOutcome(ProgrammingOutcome outcome, bool deferred, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  ScriptedOutcome scripted;
  scripted.outcome = outcome;
  scripted.deferred = deferred;
  scripted.detail = std::move(detail);
  queue_.push_back(std::move(scripted));
}

void SyntheticProgrammingBackend::SetDefaultOutcome(ProgrammingOutcome outcome, bool deferred) {
  std::lock_guard<std::mutex> lock(mutex_);
  default_outcome_.outcome = outcome;
  default_outcome_.deferred = deferred;
  default_outcome_.detail.clear();
}

void SyntheticProgrammingBackend::SetObservedPresence(BackendPresence presence, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  observed_presence_ = presence;
  observed_detail_ = std::move(detail);
}

void SyntheticProgrammingBackend::SetObservedGeneration(const RouteGeneration& generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  observed_generation_ = generation;
}

void SyntheticProgrammingBackend::SetAvailable(bool available) {
  std::lock_guard<std::mutex> lock(mutex_);
  available_ = available;
}

ProgrammingDispatch SyntheticProgrammingBackend::Dispatch(const RouteProgrammingRequest& request, bool install_family) {
  (void)install_family;
  std::lock_guard<std::mutex> lock(mutex_);
  calls_.push_back(request);

  ProgrammingResult result;
  result.attempt = request.attempt;
  result.backend = id_;

  if (!available_) {
    result.outcome = ProgrammingOutcome::BackendUnavailable;
    result.detail = "synthetic backend marked unavailable";
    ProgrammingDispatch dispatch;
    dispatch.deferred = false;
    dispatch.immediate = std::move(result);
    return dispatch;
  }

  ScriptedOutcome scripted = default_outcome_;
  if (!queue_.empty()) {
    scripted = queue_.front();
    queue_.pop_front();
  }
  result.outcome = scripted.outcome;
  result.detail = scripted.detail;

  if (scripted.deferred) {
    deferred_[request.attempt] = result;
    ProgrammingDispatch dispatch;
    dispatch.deferred = true;
    dispatch.immediate = result;
    return dispatch;
  }
  ProgrammingDispatch dispatch;
  dispatch.deferred = false;
  dispatch.immediate = std::move(result);
  return dispatch;
}

ProgrammingDispatch SyntheticProgrammingBackend::InstallRoute(const RouteProgrammingRequest& request) {
  return Dispatch(request, true);
}

ProgrammingDispatch SyntheticProgrammingBackend::ReplaceRoute(const RouteProgrammingRequest& request) {
  return Dispatch(request, true);
}

ProgrammingDispatch SyntheticProgrammingBackend::WithdrawRoute(const RouteProgrammingRequest& request) {
  return Dispatch(request, false);
}

BackendQueryResult SyntheticProgrammingBackend::QueryRoute(const RouteKey& key) {
  (void)key;
  std::lock_guard<std::mutex> lock(mutex_);
  BackendQueryResult result;
  result.backend = id_;
  result.detail = observed_detail_;
  result.observed_generation = observed_generation_;
  if (!available_) {
    result.presence = BackendPresence::Unavailable;
    return result;
  }
  result.presence = observed_presence_;
  return result;
}

bool SyntheticProgrammingBackend::CompleteDeferred(const ProgrammingAttemptId& attempt, ProgrammingOutcome outcome,
                                                   std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = deferred_.find(attempt);
  if (it == deferred_.end()) {
    return false;
  }
  it->second.outcome = outcome;
  it->second.detail = std::move(detail);
  return true;
}

bool SyntheticProgrammingBackend::HasDeferred(const ProgrammingAttemptId& attempt) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deferred_.find(attempt) != deferred_.end();
}

std::vector<ProgrammingAttemptId> SyntheticProgrammingBackend::DeferredAttempts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ProgrammingAttemptId> attempts;
  attempts.reserve(deferred_.size());
  for (const auto& entry : deferred_) {
    attempts.push_back(entry.first);
  }
  return attempts;
}

std::vector<RouteProgrammingRequest> SyntheticProgrammingBackend::RecordedCalls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return calls_;
}

std::size_t SyntheticProgrammingBackend::call_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return calls_.size();
}

void SyntheticProgrammingBackend::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  default_outcome_ = ScriptedOutcome{};
  deferred_.clear();
  calls_.clear();
  observed_presence_ = BackendPresence::Absent;
  observed_detail_.clear();
  observed_generation_ = RouteGeneration();
  available_ = true;
}

}  // namespace routefabric
