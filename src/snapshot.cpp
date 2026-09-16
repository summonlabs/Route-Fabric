#include "routefabric/snapshot.hpp"

#include <algorithm>

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

const char* to_string(RouteCurrentness currentness) noexcept {
  switch (currentness) {
    case RouteCurrentness::Current:
      return "CURRENT";
    case RouteCurrentness::NotInstalled:
      return "NOT_INSTALLED";
    case RouteCurrentness::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case RouteCurrentness::StaleEpoch:
      return "STALE_EPOCH";
    case RouteCurrentness::StalePublisher:
      return "STALE_PUBLISHER";
    case RouteCurrentness::StaleWorkerBoot:
      return "STALE_WORKER_BOOT";
    case RouteCurrentness::StalePathAuthority:
      return "STALE_PATH_AUTHORITY";
    case RouteCurrentness::StaleBackendObservation:
      return "STALE_BACKEND_OBSERVATION";
    case RouteCurrentness::FencedPublisher:
      return "FENCED_PUBLISHER";
    case RouteCurrentness::PathAuthorityRejected:
      return "PATH_AUTHORITY_REJECTED";
    case RouteCurrentness::Withdrawn:
      return "WITHDRAWN";
    case RouteCurrentness::Retired:
      return "RETIRED";
    case RouteCurrentness::Superseded:
      return "SUPERSEDED";
    case RouteCurrentness::Failed:
      return "FAILED";
    case RouteCurrentness::Revoked:
      return "REVOKED";
    case RouteCurrentness::DesiredAppliedMismatch:
      return "DESIRED_APPLIED_MISMATCH";
  }
  return "UNKNOWN";
}

bool parse_currentness(std::string_view text, RouteCurrentness& out) noexcept {
  return parse_by_name(text, RouteCurrentness::Current, RouteCurrentness::DesiredAppliedMismatch, to_string, out);
}

RouteSnapshot make_snapshot(const RouteRecord& record, RouteCurrentness currentness) {
  RouteSnapshot snapshot;
  snapshot.record = record;
  snapshot.currentness = currentness;
  snapshot.digest = semantic_digest(record);
  ByteWriter writer;
  writer.raw("routefabric.snapshot.v1");
  writer.raw(snapshot.digest.bytes());
  writer.u8(static_cast<std::uint8_t>(currentness));
  record.id.write(writer);
  record.generation.write(writer);
  record.authority_generation.write(writer);
  snapshot.id = RouteSnapshotId::from_digest(digest128("routefabric.snapshot-id", writer.buffer()));
  return snapshot;
}

RouteSnapshotSet make_snapshot_set(const std::vector<RouteRecord>& records,
                                   const std::vector<RouteCurrentness>& currentness, const CoordinatorEpoch& epoch) {
  RouteSnapshotSet set;
  set.epoch = epoch;
  const std::size_t count = std::min(records.size(), currentness.size());
  set.routes.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    set.routes.push_back(make_snapshot(records[i], currentness[i]));
  }
  std::sort(set.routes.begin(), set.routes.end(),
            [](const RouteSnapshot& a, const RouteSnapshot& b) { return a.record.id < b.record.id; });
  set.digest = digest_snapshot_set(set);
  ByteWriter writer;
  writer.raw("routefabric.snapshot-set.v1");
  writer.raw(set.digest.bytes());
  set.epoch.write(writer);
  writer.u64(static_cast<std::uint64_t>(set.routes.size()));
  set.id = RouteSnapshotId::from_digest(digest128("routefabric.snapshot-set-id", writer.buffer()));
  return set;
}

Digest128 digest_snapshot_set(const RouteSnapshotSet& set) {
  Fnv1a64 forward(0xCBF29CE484222325ull);
  Fnv1a64 backward(0x9AE16A3B2F90404Full);
  forward.update(std::string_view("routefabric.snapshot-set.v1"));
  backward.update(std::string_view("routefabric.snapshot-set.v1"));
  // The set digest is order independent: routes are visited in RouteId order.
  std::vector<const RouteSnapshot*> ordered;
  ordered.reserve(set.routes.size());
  for (const RouteSnapshot& snapshot : set.routes) {
    ordered.push_back(&snapshot);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const RouteSnapshot* a, const RouteSnapshot* b) { return a->record.id < b->record.id; });
  for (const RouteSnapshot* snapshot : ordered) {
    const Digest128 digest = semantic_digest(snapshot->record);
    forward.update(digest.bytes());
    backward.update(digest.bytes());
    const std::uint8_t currentness = static_cast<std::uint8_t>(snapshot->currentness);
    forward.update_byte(currentness);
    backward.update_byte(currentness);
  }
  return Digest128::from_u64_pair(mix64(forward.value()), mix64(backward.value()));
}

std::string RouteSnapshot::render() const {
  std::string out;
  out += "route-id: ";
  out += record.id.render();
  out += "\nkey: ";
  out += record.key.render();
  out += "\nlifecycle: ";
  out += to_string(record.lifecycle);
  out += "\ncurrentness: ";
  out += to_string(currentness);
  out += "\ngeneration: ";
  out += record.generation.render();
  out += "\nauthority-generation: ";
  out += record.authority_generation.render();
  out += "\ninvalidation-watermark: ";
  out += record.invalidation_watermark.render();
  out += "\nprogramming-generation: ";
  out += record.programming_generation.render();
  out += "\nbinding: ";
  out += record.binding.render();
  out += "\napplied: ";
  out += to_string(record.applied.classification);
  out += "\napplied-programmed-generation: ";
  out += record.applied.programmed_generation.is_valid() ? record.applied.programmed_generation.render() : "none";
  out += "\nbackend: ";
  out += record.applied.backend.render();
  out += "\nobservation: ";
  out += to_string(record.observation.classification);
  out += "\npublisher: ";
  out += record.provenance.publisher.render();
  out += "\nworker-boot: ";
  out += record.provenance.worker_boot.render();
  out += "\nepoch: ";
  out += record.provenance.epoch.render();
  out += "\npath-authority-generation: ";
  out += record.provenance.path_authority_generation.render();
  out += "\nsnapshot-id: ";
  out += id.render();
  out += "\ndigest: ";
  out += digest.to_hex();
  return out;
}

std::string RouteSnapshot::render_compact() const {
  std::string out = record.key.render();
  out += " gen=";
  out += record.generation.render();
  out += " auth-gen=";
  out += record.authority_generation.render();
  out += " lifecycle=";
  out += to_string(record.lifecycle);
  out += " currentness=";
  out += to_string(currentness);
  out += " applied=";
  out += to_string(record.applied.classification);
  out += " binding=";
  out += record.binding.render();
  return out;
}

std::string RouteSnapshotSet::render() const {
  std::string out = "epoch: ";
  out += epoch.render();
  out += "\nroute-count: ";
  out += to_decimal(routes.size());
  out += "\nsnapshot-id: ";
  out += id.render();
  out += "\ndigest: ";
  out += digest.to_hex();
  for (const RouteSnapshot& snapshot : routes) {
    out += "\n";
    out += snapshot.render_compact();
  }
  return out;
}

const RouteSnapshot* RouteSnapshotSet::find(const RouteId& route) const {
  const auto it = std::lower_bound(
      routes.begin(), routes.end(), route,
      [](const RouteSnapshot& snapshot, const RouteId& id) { return snapshot.record.id < id; });
  if (it == routes.end() || !(it->record.id == route)) {
    return nullptr;
  }
  return &(*it);
}

const char* to_string(RouteDiffField field) noexcept {
  switch (field) {
    case RouteDiffField::Presence:
      return "presence";
    case RouteDiffField::RouteId:
      return "route-id";
    case RouteDiffField::Key:
      return "key";
    case RouteDiffField::Generation:
      return "generation";
    case RouteDiffField::AuthorityGeneration:
      return "authority-generation";
    case RouteDiffField::Lifecycle:
      return "lifecycle";
    case RouteDiffField::Binding:
      return "binding";
    case RouteDiffField::NextHop:
      return "next-hop";
    case RouteDiffField::PathAuthorityGeneration:
      return "path-authority-generation";
    case RouteDiffField::AppliedClassification:
      return "applied-classification";
    case RouteDiffField::AppliedGeneration:
      return "applied-generation";
    case RouteDiffField::BackendObservation:
      return "backend-observation";
    case RouteDiffField::Publisher:
      return "publisher";
    case RouteDiffField::WorkerBoot:
      return "worker-boot";
    case RouteDiffField::Epoch:
      return "epoch";
    case RouteDiffField::Currentness:
      return "currentness";
    case RouteDiffField::Supersession:
      return "supersession";
    case RouteDiffField::Retirement:
      return "retirement";
  }
  return "unknown";
}

namespace {

void add_if_changed(std::vector<RouteDiffEntry>& entries, RouteDiffField field, const std::string& before,
                    const std::string& after) {
  if (before != after) {
    entries.push_back(RouteDiffEntry{field, before, after});
  }
}

std::string render_supersession(const SupersessionRecord& record) {
  if (record.kind == SupersessionKind::None) {
    return "none";
  }
  std::string out = to_string(record.kind);
  out += " prior=";
  out += record.prior_generation.is_valid() ? record.prior_generation.render() : "none";
  out += " successor-generation=";
  out += record.successor_generation.is_valid() ? record.successor_generation.render() : "none";
  out += " successor-lineage=";
  out += record.successor_lineage.is_valid() ? record.successor_lineage.render() : "none";
  out += " reason=";
  out += record.reason;
  return out;
}

std::string render_retirement(const RetirementRecord& record) {
  if (record.cause == RetirementCause::None) {
    return "none";
  }
  std::string out = to_string(record.cause);
  out += " generation=";
  out += record.generation.is_valid() ? record.generation.render() : "none";
  out += " reason=";
  out += record.reason;
  return out;
}

}  // namespace

std::vector<RouteDiffEntry> diff_snapshots(const RouteSnapshot& before, const RouteSnapshot& after) {
  std::vector<RouteDiffEntry> entries;
  const RouteRecord& a = before.record;
  const RouteRecord& b = after.record;
  add_if_changed(entries, RouteDiffField::Presence, "present", "present");
  add_if_changed(entries, RouteDiffField::RouteId, a.id.render(), b.id.render());
  add_if_changed(entries, RouteDiffField::Key, a.key.render(), b.key.render());
  add_if_changed(entries, RouteDiffField::Generation, a.generation.render(), b.generation.render());
  add_if_changed(entries, RouteDiffField::AuthorityGeneration, a.authority_generation.render(),
                 b.authority_generation.render());
  add_if_changed(entries, RouteDiffField::Lifecycle, to_string(a.lifecycle), to_string(b.lifecycle));
  add_if_changed(entries, RouteDiffField::Binding, a.binding.render(), b.binding.render());
  add_if_changed(entries, RouteDiffField::NextHop, a.binding.render(), b.binding.render());
  add_if_changed(entries, RouteDiffField::PathAuthorityGeneration, a.provenance.path_authority_generation.render(),
                 b.provenance.path_authority_generation.render());
  add_if_changed(entries, RouteDiffField::AppliedClassification, to_string(a.applied.classification),
                 to_string(b.applied.classification));
  add_if_changed(entries, RouteDiffField::AppliedGeneration,
                 a.applied.programmed_generation.is_valid() ? a.applied.programmed_generation.render() : "none",
                 b.applied.programmed_generation.is_valid() ? b.applied.programmed_generation.render() : "none");
  add_if_changed(entries, RouteDiffField::BackendObservation, to_string(a.observation.classification),
                 to_string(b.observation.classification));
  add_if_changed(entries, RouteDiffField::Publisher, a.provenance.publisher.render(), b.provenance.publisher.render());
  add_if_changed(entries, RouteDiffField::WorkerBoot, a.provenance.worker_boot.render(),
                 b.provenance.worker_boot.render());
  add_if_changed(entries, RouteDiffField::Epoch, a.provenance.epoch.render(), b.provenance.epoch.render());
  add_if_changed(entries, RouteDiffField::Currentness, to_string(before.currentness), to_string(after.currentness));
  add_if_changed(entries, RouteDiffField::Supersession, render_supersession(a.supersession),
                 render_supersession(b.supersession));
  add_if_changed(entries, RouteDiffField::Retirement, render_retirement(a.retirement), render_retirement(b.retirement));
  return entries;
}

std::vector<RouteDiffEntry> diff_snapshot_sets(const RouteSnapshotSet& before, const RouteSnapshotSet& after,
                                               const RouteId& route) {
  const RouteSnapshot* a = before.find(route);
  const RouteSnapshot* b = after.find(route);
  std::vector<RouteDiffEntry> entries;
  if (a == nullptr && b == nullptr) {
    return entries;
  }
  if (a == nullptr) {
    entries.push_back(RouteDiffEntry{RouteDiffField::Presence, "absent", "present"});
    entries.push_back(RouteDiffEntry{RouteDiffField::Key, "", b->record.key.render()});
    entries.push_back(RouteDiffEntry{RouteDiffField::Lifecycle, "", to_string(b->record.lifecycle)});
    entries.push_back(RouteDiffEntry{RouteDiffField::Currentness, "", to_string(b->currentness)});
    return entries;
  }
  if (b == nullptr) {
    entries.push_back(RouteDiffEntry{RouteDiffField::Presence, "present", "absent"});
    entries.push_back(RouteDiffEntry{RouteDiffField::Key, a->record.key.render(), ""});
    entries.push_back(RouteDiffEntry{RouteDiffField::Lifecycle, to_string(a->record.lifecycle), ""});
    entries.push_back(RouteDiffEntry{RouteDiffField::Currentness, to_string(a->currentness), ""});
    return entries;
  }
  return diff_snapshots(*a, *b);
}

std::string render_diff(const RouteId& route, const std::vector<RouteDiffEntry>& entries) {
  std::string out = "route-id: ";
  out += route.render();
  if (entries.empty()) {
    out += "\nno change";
    return out;
  }
  for (const RouteDiffEntry& entry : entries) {
    out += "\n";
    out += to_string(entry.field);
    out += ": ";
    out += entry.before;
    out += " -> ";
    out += entry.after;
  }
  return out;
}

std::string RouteExplanation::render() const {
  std::string out = snapshot.render();
  out += "\nauthoritative: ";
  out += authoritative ? "yes" : "no";
  out += "\nauthority-reason: ";
  out += authority_reason;
  out += "\ncurrentness-reason: ";
  out += currentness_reason;
  out += "\nlifecycle-reason: ";
  out += lifecycle_reason;
  out += "\npath-authority-reason: ";
  out += path_authority_reason;
  out += "\nbackend-reason: ";
  out += backend_reason;
  out += "\nreconciliation-reason: ";
  out += reconciliation_reason;
  out += "\nsupersession-reason: ";
  out += supersession_reason;
  out += "\nretirement-reason: ";
  out += retirement_reason;
  return out;
}

}  // namespace routefabric
