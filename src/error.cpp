#include "routefabric/error.hpp"

namespace routefabric {

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return "ok";
    case StatusCode::InvalidArgument:
      return "invalid-argument";
    case StatusCode::MalformedEncoding:
      return "malformed-encoding";
    case StatusCode::NotFound:
      return "not-found";
    case StatusCode::AlreadyExists:
      return "already-exists";
    case StatusCode::Conflict:
      return "conflict";
    case StatusCode::StaleEpoch:
      return "stale-epoch";
    case StatusCode::StaleWorkerBoot:
      return "stale-worker-boot";
    case StatusCode::StaleGeneration:
      return "stale-generation";
    case StatusCode::Unauthorized:
      return "unauthorized";
    case StatusCode::ScopeViolation:
      return "scope-violation";
    case StatusCode::PathAuthorityRejected:
      return "path-authority-rejected";
    case StatusCode::PathAuthorityStale:
      return "path-authority-stale";
    case StatusCode::LifecycleViolation:
      return "lifecycle-violation";
    case StatusCode::Retired:
      return "retired";
    case StatusCode::Revoked:
      return "revoked";
    case StatusCode::BackendUnavailable:
      return "backend-unavailable";
    case StatusCode::BackendFailure:
      return "backend-failure";
    case StatusCode::AmbiguousOutcome:
      return "ambiguous-outcome";
    case StatusCode::LimitExceeded:
      return "limit-exceeded";
    case StatusCode::PersistenceFailure:
      return "persistence-failure";
    case StatusCode::ProtocolViolation:
      return "protocol-violation";
    case StatusCode::IntegrityFailure:
      return "integrity-failure";
    case StatusCode::Unsupported:
      return "unsupported";
    case StatusCode::ReentrancyViolation:
      return "reentrancy-violation";
    case StatusCode::Internal:
      return "internal";
    case StatusCode::NotRegistered:
      return "not-registered";
    case StatusCode::Fenced:
      return "fenced";
    case StatusCode::ShuttingDown:
      return "shutting-down";
    case StatusCode::NotOpen:
      return "not-open";
  }
  return "unknown";
}

std::string Error::render() const {
  std::string out = to_string(code_);
  if (!detail_.empty()) {
    out += ": ";
    out += detail_;
  }
  return out;
}

}  // namespace routefabric
