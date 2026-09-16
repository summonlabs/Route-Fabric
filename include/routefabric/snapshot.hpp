#ifndef ROUTEFABRIC_SNAPSHOT_HPP
#define ROUTEFABRIC_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "routefabric/ids.hpp"
#include "routefabric/record.hpp"

namespace routefabric {

// Explicit route currentness. Non-current states are never collapsed into a
// single false: an operator can tell why a route is not current.
enum class RouteCurrentness : std::uint8_t {
  Current = 1,
  NotInstalled = 2,
  RevalidationRequired = 3,
  StaleEpoch = 4,
  StalePublisher = 5,
  StaleWorkerBoot = 6,
  StalePathAuthority = 7,
  StaleBackendObservation = 8,
  FencedPublisher = 9,
  PathAuthorityRejected = 10,
  Withdrawn = 11,
  Retired = 12,
  Superseded = 13,
  Failed = 14,
  Revoked = 15,
  DesiredAppliedMismatch = 16,
};

const char* to_string(RouteCurrentness currentness) noexcept;
bool parse_currentness(std::string_view text, RouteCurrentness& out) noexcept;

// True only for RouteCurrentness::Current.
constexpr bool is_current(RouteCurrentness currentness) noexcept {
  return currentness == RouteCurrentness::Current;
}

// Immutable, content-addressed route snapshot. Snapshots remain inspectable
// after the route changes but never grant current authority.
struct RouteSnapshot {
  RouteSnapshotId id;
  Digest128 digest;
  RouteCurrentness currentness = RouteCurrentness::NotInstalled;
  RouteRecord record;

  // Deterministic multi-line rendering used by the CLI and by tests.
  std::string render() const;
  // One-line, script-friendly rendering.
  std::string render_compact() const;
};

// Immutable snapshot of the whole route runtime, ordered by RouteId.
struct RouteSnapshotSet {
  RouteSnapshotId id;
  Digest128 digest;
  CoordinatorEpoch epoch;
  std::vector<RouteSnapshot> routes;

  std::size_t size() const noexcept { return routes.size(); }
  std::string render() const;
  // Looks up a route by identity using binary search over the ordered snapshot.
  const RouteSnapshot* find(const RouteId& id) const;
};

// Builds a content-addressed snapshot of one record.
RouteSnapshot make_snapshot(const RouteRecord& record, RouteCurrentness currentness);

// Builds a content-addressed snapshot set. The input is copied and sorted by
// RouteId, so identical state always produces the identical snapshot identity.
RouteSnapshotSet make_snapshot_set(const std::vector<RouteRecord>& records,
                                   const std::vector<RouteCurrentness>& currentness,
                                   const CoordinatorEpoch& epoch);

Digest128 digest_snapshot_set(const RouteSnapshotSet& set);

// ---------------------------------------------------------------------------
// Diffs
// ---------------------------------------------------------------------------

// Diff fields in their stable reporting order.
enum class RouteDiffField : std::uint8_t {
  Presence = 1,
  RouteId = 2,
  Key = 3,
  Generation = 4,
  AuthorityGeneration = 5,
  Lifecycle = 6,
  Binding = 7,
  NextHop = 8,
  PathAuthorityGeneration = 9,
  AppliedClassification = 10,
  AppliedGeneration = 11,
  BackendObservation = 12,
  Publisher = 13,
  WorkerBoot = 14,
  Epoch = 15,
  Currentness = 16,
  Supersession = 17,
  Retirement = 18,
};

const char* to_string(RouteDiffField field) noexcept;

struct RouteDiffEntry {
  RouteDiffField field = RouteDiffField::Presence;
  std::string before;
  std::string after;
};

// Deterministic diff between two snapshots of the same route identity. Entries
// are always emitted in RouteDiffField order.
std::vector<RouteDiffEntry> diff_snapshots(const RouteSnapshot& before, const RouteSnapshot& after);

// Deterministic diff between two snapshot sets, ordered by RouteId and then by
// RouteDiffField. Routes present in only one set are reported through Presence.
std::vector<RouteDiffEntry> diff_snapshot_sets(const RouteSnapshotSet& before, const RouteSnapshotSet& after,
                                               const RouteId& route);

std::string render_diff(const RouteId& route, const std::vector<RouteDiffEntry>& entries);

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

// Structured answer to the operator questions: why is this route authoritative
// or not, which publisher and boot produced it, which epoch and path authority
// generation support it, why is it revalidation-required or withdrawn, and what
// did the backend last report.
struct RouteExplanation {
  RouteSnapshot snapshot;
  bool authoritative = false;
  std::string authority_reason;
  std::string currentness_reason;
  std::string lifecycle_reason;
  std::string path_authority_reason;
  std::string backend_reason;
  std::string reconciliation_reason;
  std::string supersession_reason;
  std::string retirement_reason;

  std::string render() const;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_SNAPSHOT_HPP
