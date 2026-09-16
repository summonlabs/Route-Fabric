#ifndef ROUTEFABRIC_RECORD_HPP
#define ROUTEFABRIC_RECORD_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "routefabric/encoding.hpp"
#include "routefabric/error.hpp"
#include "routefabric/hash.hpp"
#include "routefabric/ids.hpp"
#include "routefabric/lifecycle.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/route_key.hpp"

namespace routefabric {

// Free-text diagnostic fields are bounded and are excluded from semantic
// digests: they describe why something happened, never what is authoritative.
inline constexpr std::size_t kMaxDiagnosticChars = 256;

// ---------------------------------------------------------------------------
// Next-hop and path binding
// ---------------------------------------------------------------------------

enum class NextHopKind : std::uint8_t {
  DirectEndpoint = 1,
  Router = 2,
  Port = 3,
  NextHopGroup = 4,
};

const char* to_string(NextHopKind kind) noexcept;
bool parse_next_hop_kind(std::string_view text, NextHopKind& out) noexcept;

// A next hop binds the identity of the referenced entity *and* the generation of
// that entity as published by the owning runtime, so a replacement can never
// alias a stale entity.
struct NextHopBinding {
  NextHopKind kind = NextHopKind::DirectEndpoint;
  NextHopId next_hop;
  NextHopGroupId next_hop_group;
  PolicyGeneration entity_generation;

  bool is_valid() const noexcept;

  friend bool operator==(const NextHopBinding&, const NextHopBinding&) = default;
  friend auto operator<=>(const NextHopBinding&, const NextHopBinding&) = default;
};

enum class BindingKind : std::uint8_t {
  NextHop = 1,
  AuthorizedPath = 2,
};

const char* to_string(BindingKind kind) noexcept;
bool parse_binding_kind(std::string_view text, BindingKind& out) noexcept;

// Desired binding of the route. Route Fabric never computes a path: a
// path-backed route must name an exact path and the exact Path Authority
// generation that authorized it.
struct RouteBinding {
  BindingKind kind = BindingKind::NextHop;
  NextHopBinding next_hop;
  PathId path;
  PathAuthorityGeneration path_authority_generation;
  PolicyGeneration policy_generation;

  bool is_valid() const noexcept;
  std::string render() const;

  friend bool operator==(const RouteBinding&, const RouteBinding&) = default;
  friend auto operator<=>(const RouteBinding&, const RouteBinding&) = default;
};

// ---------------------------------------------------------------------------
// Applied state
// ---------------------------------------------------------------------------

// Classification of what the programming backend actually reported. Backend
// success is never conflated with the commit of the desired route record.
enum class AppliedClassification : std::uint8_t {
  Unknown = 1,
  Pending = 2,
  Applied = 3,
  IdempotentAlreadyApplied = 4,
  Rejected = 5,
  NotSupported = 6,
  RetryableFailure = 7,
  PermanentFailure = 8,
  Ambiguous = 9,
  Withdrawn = 10,
  WithdrawFailed = 11,
  BackendUnavailable = 12,
};

const char* to_string(AppliedClassification classification) noexcept;
bool parse_applied_classification(std::string_view text, AppliedClassification& out) noexcept;

struct AppliedState {
  AppliedClassification classification = AppliedClassification::Unknown;
  ProgrammingAttemptId attempt;
  ProgrammingGeneration programming_generation;
  RouteGeneration programmed_generation;
  RouteAuthorityGeneration programmed_authority_generation;
  BackendId backend;
  std::string detail;

  bool has_outcome() const noexcept {
    return classification != AppliedClassification::Unknown && classification != AppliedClassification::Pending;
  }
  bool reports_applied() const noexcept {
    return classification == AppliedClassification::Applied ||
           classification == AppliedClassification::IdempotentAlreadyApplied;
  }
  bool reports_not_present() const noexcept {
    return classification == AppliedClassification::Withdrawn;
  }

  friend bool operator==(const AppliedState&, const AppliedState&) = default;
};

// Observed backend state. Reconciliation records evidence; it never replaces
// authority.
enum class ObservationClass : std::uint8_t {
  Unknown = 1,
  Matched = 2,
  Missing = 3,
  Diverged = 4,
  Extra = 5,
  Unavailable = 6,
};

const char* to_string(ObservationClass classification) noexcept;
bool parse_observation_class(std::string_view text, ObservationClass& out) noexcept;

struct BackendObservation {
  ObservationClass classification = ObservationClass::Unknown;
  ProgrammingAttemptId attempt;
  RouteGeneration observed_generation;
  BackendId backend;
  std::string detail;

  friend bool operator==(const BackendObservation&, const BackendObservation&) = default;
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

enum class ProvenanceSourceClass : std::uint8_t {
  Publisher = 1,
  CoordinatorRecovery = 2,
  AdministrativeAction = 3,
  Reconciliation = 4,
  LineageMint = 5,
};

const char* to_string(ProvenanceSourceClass source) noexcept;
bool parse_provenance_source(std::string_view text, ProvenanceSourceClass& out) noexcept;

// Structured provenance. Every mutation carries the full authority context that
// produced it; a free-form source string is never sufficient.
struct Provenance {
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  ProvenanceSourceClass source_class = ProvenanceSourceClass::Publisher;
  RouteGeneration source_generation;
  MutationAttemptId mutation_attempt;
  PolicyGeneration policy_generation;
  PathAuthorityGeneration path_authority_generation;

  friend bool operator==(const Provenance&, const Provenance&) = default;
};

// ---------------------------------------------------------------------------
// Lineage, supersession, retirement, revocation
// ---------------------------------------------------------------------------

struct LineageEntry {
  RouteGeneration generation;
  RouteGeneration prior_generation;
  RouteEvent event = RouteEvent::BeginValidation;
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  BindingKind binding_kind = BindingKind::NextHop;
  PathId path;
  PathAuthorityGeneration path_authority_generation;
  PublisherId publisher;
  CoordinatorEpoch epoch;

  friend bool operator==(const LineageEntry&, const LineageEntry&) = default;
};

enum class SupersessionKind : std::uint8_t {
  None = 0,
  Replacement = 1,
  LineageTakeover = 2,
};

const char* to_string(SupersessionKind kind) noexcept;

struct SupersessionRecord {
  SupersessionKind kind = SupersessionKind::None;
  RouteGeneration prior_generation;
  RouteGeneration successor_generation;
  RouteId successor_lineage;
  std::string reason;
  CoordinatorEpoch epoch;

  friend bool operator==(const SupersessionRecord&, const SupersessionRecord&) = default;
};

enum class RetirementCause : std::uint8_t {
  None = 0,
  Administrative = 1,
  Revoked = 2,
  SupersededByLineage = 3,
  EpochInvalidated = 4,
};

const char* to_string(RetirementCause cause) noexcept;

struct RetirementRecord {
  RetirementCause cause = RetirementCause::None;
  std::string reason;
  CoordinatorEpoch epoch;
  RouteGeneration generation;
  MutationAttemptId attempt;

  friend bool operator==(const RetirementRecord&, const RetirementRecord&) = default;
};

// Durable, generation-bound, reason-coded administrative revocation.
struct RevocationRecord {
  RouteId route;
  RouteKey key;
  RouteGeneration generation;
  CoordinatorEpoch epoch;
  MutationAttemptId attempt;
  std::string reason;

  friend bool operator==(const RevocationRecord&, const RevocationRecord&) = default;
};

// ---------------------------------------------------------------------------
// Route record
// ---------------------------------------------------------------------------

// The authoritative route record. One record per route key; the record is
// mutated in place under the runtime mutation lock and its RouteGeneration
// advances on every semantic mutation.
struct RouteRecord {
  RouteId id;
  RouteKey key;
  RouteGeneration generation;                       // semantic content generation
  RouteAuthorityGeneration authority_generation;    // authority binding generation
  RouteAuthorityGeneration invalidation_watermark;  // last authority generation that invalidated the route
  ProgrammingGeneration programming_generation;     // highest dispatched programming generation
  RouteLifecycle lifecycle = RouteLifecycle::Declared;
  RouteBinding binding;
  AppliedState applied;
  BackendObservation observation;
  Provenance provenance;
  SupersessionRecord supersession;
  RetirementRecord retirement;
  std::vector<LineageEntry> history;

  bool is_retired() const noexcept { return lifecycle == RouteLifecycle::Retired; }
  bool blocks_republication() const noexcept {
    return lifecycle == RouteLifecycle::Retired || lifecycle == RouteLifecycle::Superseded;
  }

  friend bool operator==(const RouteRecord&, const RouteRecord&) = default;
};

// Semantic digest of one route. Diagnostic detail strings, arrival order and
// process-local counters are excluded.
Digest128 semantic_digest(const RouteRecord& record);

void write_route_record(ByteWriter& writer, const RouteRecord& record);
bool read_route_record(ByteReader& reader, const Limits& limits, RouteRecord& out, std::string& why);

void write_route_binding(ByteWriter& writer, const RouteBinding& binding);
bool read_route_binding(ByteReader& reader, RouteBinding& out);

void write_revocation(ByteWriter& writer, const RevocationRecord& record);
bool read_revocation(ByteReader& reader, const Limits& limits, RevocationRecord& out, std::string& why);

// Structural validation applied to every record that enters the runtime, whether
// it arrives from a publisher, from reconciliation or from the persistence
// layer.
Status validate_route_record(const RouteRecord& record, const Limits& limits);

}  // namespace routefabric

#endif  // ROUTEFABRIC_RECORD_HPP
