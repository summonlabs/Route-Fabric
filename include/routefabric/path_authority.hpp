#ifndef ROUTEFABRIC_PATH_AUTHORITY_HPP
#define ROUTEFABRIC_PATH_AUTHORITY_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "routefabric/error.hpp"
#include "routefabric/ids.hpp"

namespace routefabric {

// Authorization state reported by Path Authority for one exact path.
//
// Path Authority owns whether an exact supplied path is legally usable. Route
// Fabric never computes, searches for, or repairs a path; it consumes this
// answer and refuses to keep a route installable when the answer is anything
// other than Usable.
enum class PathAuthorization : std::uint8_t {
  Usable = 1,
  RevalidationRequired = 2,
  Rejected = 3,
  Revoked = 4,
  Stale = 5,
  Retired = 6,
};

const char* to_string(PathAuthorization state) noexcept;
bool parse_path_authorization(std::string_view text, PathAuthorization& out) noexcept;

struct PathAuthorityResult {
  PathId path;
  PathAuthorityGeneration generation;
  PathAuthorization state = PathAuthorization::Rejected;

  bool usable() const noexcept { return state == PathAuthorization::Usable; }
};

// Path Authority consumption interface.
//
// Contract:
//   * Query is synchronous and must not block on network or disk I/O.
//   * Query must be safe to call while the Route Fabric mutation lock is held.
//   * Query must not call back into RouteFabricRuntime. A re-entrant call is
//     detected and reported as a ReentrancyViolation instead of deadlocking.
class IPathAuthority {
 public:
  virtual ~IPathAuthority() = default;
  virtual PathAuthorityResult Query(const PathId& path) const = 0;
};

// In-memory Path Authority used by the shipped tests, examples and coordinator
// harness. This implementation is SYNTHETIC: it is not a real path-computation
// or path-legality runtime. Paths that are unknown to it are reported as
// Rejected at generation 1, so an unknown path can never authorize a route.
class SyntheticPathAuthority final : public IPathAuthority {
 public:
  SyntheticPathAuthority() = default;

  // Installs or replaces the authorization state of a path.
  Status SetPath(const PathId& path, const PathAuthorityGeneration& generation, PathAuthorization state);

  // Advances the generation of a known path and sets its new state. This is the
  // precise invalidation trigger consumed by the runtime.
  Expected<PathAuthorityGeneration> Invalidate(const PathId& path, PathAuthorization state);

  // Removes a path entirely; subsequent queries report Rejected.
  bool Remove(const PathId& path);

  PathAuthorityResult Query(const PathId& path) const override;

  std::size_t size() const;

 private:
  struct Entry {
    PathAuthorityGeneration generation;
    PathAuthorization state = PathAuthorization::Rejected;
  };
  mutable std::mutex mutex_;
  std::map<PathId, Entry> paths_;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_PATH_AUTHORITY_HPP
