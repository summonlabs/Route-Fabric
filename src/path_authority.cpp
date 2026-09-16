#include "routefabric/path_authority.hpp"

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

const char* to_string(PathAuthorization state) noexcept {
  switch (state) {
    case PathAuthorization::Usable:
      return "USABLE";
    case PathAuthorization::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case PathAuthorization::Rejected:
      return "REJECTED";
    case PathAuthorization::Revoked:
      return "REVOKED";
    case PathAuthorization::Stale:
      return "STALE";
    case PathAuthorization::Retired:
      return "RETIRED";
  }
  return "UNKNOWN";
}

bool parse_path_authorization(std::string_view text, PathAuthorization& out) noexcept {
  return parse_by_name(text, PathAuthorization::Usable, PathAuthorization::Retired, to_string, out);
}

Status SyntheticPathAuthority::SetPath(const PathId& path, const PathAuthorityGeneration& generation,
                                       PathAuthorization state) {
  if (!path.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "path identity must not be zero");
  }
  if (!generation.is_valid()) {
    return make_error(StatusCode::InvalidArgument, "path authority generation must be at least 1");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  paths_[path] = Entry{generation, state};
  return ok_status();
}

Expected<PathAuthorityGeneration> SyntheticPathAuthority::Invalidate(const PathId& path, PathAuthorization state) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = paths_.find(path);
  if (it == paths_.end()) {
    return make_error<PathAuthorityGeneration>(StatusCode::NotFound, "unknown path");
  }
  const Expected<PathAuthorityGeneration> next = it->second.generation.next();
  if (!next) {
    return next;
  }
  it->second.generation = next.value();
  it->second.state = state;
  return next.value();
}

bool SyntheticPathAuthority::Remove(const PathId& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return paths_.erase(path) > 0;
}

PathAuthorityResult SyntheticPathAuthority::Query(const PathId& path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  PathAuthorityResult result;
  result.path = path;
  const auto it = paths_.find(path);
  if (it == paths_.end()) {
    result.generation = PathAuthorityGeneration::from_value(1);
    result.state = PathAuthorization::Rejected;
    return result;
  }
  result.generation = it->second.generation;
  result.state = it->second.state;
  return result;
}

std::size_t SyntheticPathAuthority::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return paths_.size();
}

}  // namespace routefabric
