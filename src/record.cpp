#include "routefabric/record.hpp"

#include <array>

namespace routefabric {
namespace {

// Record encoding tag. Incremented whenever the record encoding changes.
constexpr std::uint8_t kRecordEncodingTag = 1;

template <typename Enum>
bool parse_enum_by_name(std::string_view text, Enum first, Enum last, const char* (*render)(Enum) noexcept, Enum& out) {
  for (std::uint8_t raw = static_cast<std::uint8_t>(first); raw <= static_cast<std::uint8_t>(last); ++raw) {
    const auto candidate = static_cast<Enum>(raw);
    if (text == render(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

template <typename Enum>
bool read_enum(ByteReader& reader, std::uint8_t first, std::uint8_t last, Enum& out) {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return false;
  }
  if (raw < first || raw > last) {
    return false;
  }
  out = static_cast<Enum>(raw);
  return true;
}

// Optional fixed-size identity: written as raw bytes and accepted as zero when
// absent. Required identities use Id::read, which rejects the zero value.
template <typename Id>
void write_optional_fixed(ByteWriter& writer, const Id& id) {
  writer.raw(id.bytes());
}

template <typename Id>
bool read_optional_fixed(ByteReader& reader, Id& out) {
  std::span<const std::uint8_t> raw;
  if (!reader.raw(Id::kByteCount, raw)) {
    return false;
  }
  std::array<std::uint8_t, Id::kByteCount> bytes{};
  for (std::size_t i = 0; i < Id::kByteCount; ++i) {
    bytes[i] = raw[i];
  }
  out = Id(bytes);
  return true;
}

// Optional counter identity: an explicit presence flag keeps the encoding
// fixed-width while distinguishing "absent" from "zero".
template <typename Id>
void write_optional_counter(ByteWriter& writer, const Id& id) {
  if (id.is_valid()) {
    writer.u8(1);
    id.write(writer);
  } else {
    writer.u8(0);
    writer.u64(0);
  }
}

template <typename Id>
bool read_optional_counter(ByteReader& reader, Id& out) {
  std::uint8_t present = 0;
  std::uint64_t raw = 0;
  if (!reader.u8(present) || !reader.u64(raw)) {
    return false;
  }
  if (present == 0) {
    if (raw != 0) {
      return false;
    }
    out = Id();
    return true;
  }
  if (present != 1 || raw == 0) {
    return false;
  }
  out = Id::from_value(raw);
  return true;
}


void write_applied(ByteWriter& writer, const AppliedState& applied) {
  writer.u8(static_cast<std::uint8_t>(applied.classification));
  write_optional_fixed(writer, applied.attempt);
  write_optional_counter(writer, applied.programming_generation);
  write_optional_counter(writer, applied.programmed_generation);
  write_optional_counter(writer, applied.programmed_authority_generation);
  applied.backend.write(writer);
  writer.string(applied.detail);
}

bool read_applied(ByteReader& reader, AppliedState& out) {
  AppliedState applied;
  if (!read_enum(reader, 1, 12, applied.classification)) {
    return false;
  }
  if (!read_optional_fixed(reader, applied.attempt)) {
    return false;
  }
  if (!read_optional_counter(reader, applied.programming_generation)) {
    return false;
  }
  if (!read_optional_counter(reader, applied.programmed_generation)) {
    return false;
  }
  if (!read_optional_counter(reader, applied.programmed_authority_generation)) {
    return false;
  }
  if (!BackendId::read(reader, applied.backend)) {
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, applied.detail)) {
    return false;
  }
  out = std::move(applied);
  return true;
}

void write_observation(ByteWriter& writer, const BackendObservation& observation) {
  writer.u8(static_cast<std::uint8_t>(observation.classification));
  write_optional_fixed(writer, observation.attempt);
  write_optional_counter(writer, observation.observed_generation);
  observation.backend.write(writer);
  writer.string(observation.detail);
}

bool read_observation(ByteReader& reader, BackendObservation& out) {
  BackendObservation observation;
  if (!read_enum(reader, 1, 6, observation.classification)) {
    return false;
  }
  if (!read_optional_fixed(reader, observation.attempt)) {
    return false;
  }
  if (!read_optional_counter(reader, observation.observed_generation)) {
    return false;
  }
  if (!BackendId::read(reader, observation.backend)) {
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, observation.detail)) {
    return false;
  }
  out = std::move(observation);
  return true;
}

void write_provenance(ByteWriter& writer, const Provenance& provenance) {
  provenance.publisher.write(writer);
  provenance.worker_boot.write(writer);
  provenance.epoch.write(writer);
  writer.u8(static_cast<std::uint8_t>(provenance.source_class));
  provenance.source_generation.write(writer);
  provenance.mutation_attempt.write(writer);
  provenance.policy_generation.write(writer);
  provenance.path_authority_generation.write(writer);
}

bool read_provenance(ByteReader& reader, Provenance& out) {
  Provenance provenance;
  if (!PublisherId::read(reader, provenance.publisher)) {
    return false;
  }
  if (!WorkerBootId::read(reader, provenance.worker_boot)) {
    return false;
  }
  if (!CoordinatorEpoch::read(reader, provenance.epoch)) {
    return false;
  }
  if (!read_enum(reader, 1, 5, provenance.source_class)) {
    return false;
  }
  if (!RouteGeneration::read(reader, provenance.source_generation)) {
    return false;
  }
  if (!MutationAttemptId::read(reader, provenance.mutation_attempt)) {
    return false;
  }
  if (!PolicyGeneration::read(reader, provenance.policy_generation)) {
    return false;
  }
  if (!PathAuthorityGeneration::read(reader, provenance.path_authority_generation)) {
    return false;
  }
  out = std::move(provenance);
  return true;
}

void write_supersession(ByteWriter& writer, const SupersessionRecord& record) {
  writer.u8(static_cast<std::uint8_t>(record.kind));
  write_optional_counter(writer, record.prior_generation);
  write_optional_counter(writer, record.successor_generation);
  write_optional_fixed(writer, record.successor_lineage);
  writer.string(record.reason);
  write_optional_counter(writer, record.epoch);
}

bool read_supersession(ByteReader& reader, SupersessionRecord& out) {
  SupersessionRecord record;
  if (!read_enum(reader, 0, 2, record.kind)) {
    return false;
  }
  if (!read_optional_counter(reader, record.prior_generation)) {
    return false;
  }
  if (!read_optional_counter(reader, record.successor_generation)) {
    return false;
  }
  if (!read_optional_fixed(reader, record.successor_lineage)) {
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, record.reason)) {
    return false;
  }
  if (!read_optional_counter(reader, record.epoch)) {
    return false;
  }
  out = std::move(record);
  return true;
}

void write_retirement(ByteWriter& writer, const RetirementRecord& record) {
  writer.u8(static_cast<std::uint8_t>(record.cause));
  writer.string(record.reason);
  write_optional_counter(writer, record.epoch);
  write_optional_counter(writer, record.generation);
  write_optional_fixed(writer, record.attempt);
}

bool read_retirement(ByteReader& reader, RetirementRecord& out) {
  RetirementRecord record;
  if (!read_enum(reader, 0, 4, record.cause)) {
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, record.reason)) {
    return false;
  }
  if (!read_optional_counter(reader, record.epoch)) {
    return false;
  }
  if (!read_optional_counter(reader, record.generation)) {
    return false;
  }
  if (!read_optional_fixed(reader, record.attempt)) {
    return false;
  }
  out = std::move(record);
  return true;
}

void write_history_entry(ByteWriter& writer, const LineageEntry& entry) {
  entry.generation.write(writer);
  write_optional_counter(writer, entry.prior_generation);
  writer.u8(static_cast<std::uint8_t>(entry.event));
  writer.u8(static_cast<std::uint8_t>(entry.lifecycle));
  writer.u8(static_cast<std::uint8_t>(entry.binding_kind));
  write_optional_fixed(writer, entry.path);
  write_optional_counter(writer, entry.path_authority_generation);
  entry.publisher.write(writer);
  entry.epoch.write(writer);
}

bool read_history_entry(ByteReader& reader, LineageEntry& out) {
  LineageEntry entry;
  if (!RouteGeneration::read(reader, entry.generation)) {
    return false;
  }
  if (!read_optional_counter(reader, entry.prior_generation)) {
    return false;
  }
  std::uint8_t event = 0;
  std::uint8_t lifecycle = 0;
  std::uint8_t binding_kind = 0;
  if (!reader.u8(event) || !reader.u8(lifecycle) || !reader.u8(binding_kind)) {
    return false;
  }
  if (!is_route_event_value(event) || !is_lifecycle_value(lifecycle)) {
    return false;
  }
  if (binding_kind < static_cast<std::uint8_t>(BindingKind::NextHop) ||
      binding_kind > static_cast<std::uint8_t>(BindingKind::AuthorizedPath)) {
    return false;
  }
  entry.event = static_cast<RouteEvent>(event);
  entry.lifecycle = static_cast<RouteLifecycle>(lifecycle);
  entry.binding_kind = static_cast<BindingKind>(binding_kind);
  if (!read_optional_fixed(reader, entry.path)) {
    return false;
  }
  if (!read_optional_counter(reader, entry.path_authority_generation)) {
    return false;
  }
  if (!PublisherId::read(reader, entry.publisher)) {
    return false;
  }
  if (!CoordinatorEpoch::read(reader, entry.epoch)) {
    return false;
  }
  out = std::move(entry);
  return true;
}

// Hashes one record's semantic content. Diagnostic strings are deliberately
// excluded so that equivalent semantic state hashes identically regardless of
// how it was reached or what evidence text accompanied it.
void hash_record(const RouteRecord& record, Fnv1a64& forward, Fnv1a64& backward) {
  ByteWriter writer;
  record.id.write(writer);
  record.key.write(writer);
  record.generation.write(writer);
  record.authority_generation.write(writer);
  record.invalidation_watermark.write(writer);
  record.programming_generation.write(writer);
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  write_route_binding(writer, record.binding);
  writer.u8(static_cast<std::uint8_t>(record.applied.classification));
  // Programming attempt identities are generated per dispatch and carry no
  // authority: two equivalent route states must hash identically even when they
  // were produced by different processes, so they are excluded here.
  write_optional_counter(writer, record.applied.programming_generation);
  write_optional_counter(writer, record.applied.programmed_generation);
  write_optional_counter(writer, record.applied.programmed_authority_generation);
  record.applied.backend.write(writer);
  writer.u8(static_cast<std::uint8_t>(record.observation.classification));
  write_optional_counter(writer, record.observation.observed_generation);
  record.observation.backend.write(writer);
  write_provenance(writer, record.provenance);
  write_supersession(writer, record.supersession);
  write_retirement(writer, record.retirement);
  writer.u32(static_cast<std::uint32_t>(record.history.size()));
  for (const LineageEntry& entry : record.history) {
    write_history_entry(writer, entry);
  }
  forward.update(writer.buffer());
  const std::vector<std::uint8_t>& bytes = writer.buffer();
  for (std::size_t i = bytes.size(); i-- > 0;) {
    backward.update_byte(bytes[i]);
  }
}

}  // namespace

void write_route_binding(ByteWriter& writer, const RouteBinding& binding) {
  writer.u8(static_cast<std::uint8_t>(binding.kind));
  writer.u8(static_cast<std::uint8_t>(binding.next_hop.kind));
  write_optional_fixed(writer, binding.next_hop.next_hop);
  write_optional_fixed(writer, binding.next_hop.next_hop_group);
  binding.next_hop.entity_generation.write(writer);
  write_optional_fixed(writer, binding.path);
  write_optional_counter(writer, binding.path_authority_generation);
  binding.policy_generation.write(writer);
}

bool read_route_binding(ByteReader& reader, RouteBinding& out) {
  RouteBinding binding;
  std::uint8_t kind = 0;
  std::uint8_t next_hop_kind = 0;
  if (!reader.u8(kind) || !reader.u8(next_hop_kind)) {
    return false;
  }
  if (kind < static_cast<std::uint8_t>(BindingKind::NextHop) ||
      kind > static_cast<std::uint8_t>(BindingKind::AuthorizedPath)) {
    return false;
  }
  if (next_hop_kind < static_cast<std::uint8_t>(NextHopKind::DirectEndpoint) ||
      next_hop_kind > static_cast<std::uint8_t>(NextHopKind::NextHopGroup)) {
    return false;
  }
  binding.kind = static_cast<BindingKind>(kind);
  binding.next_hop.kind = static_cast<NextHopKind>(next_hop_kind);
  if (!read_optional_fixed(reader, binding.next_hop.next_hop)) {
    return false;
  }
  if (!read_optional_fixed(reader, binding.next_hop.next_hop_group)) {
    return false;
  }
  if (!PolicyGeneration::read(reader, binding.next_hop.entity_generation)) {
    return false;
  }
  if (!read_optional_fixed(reader, binding.path)) {
    return false;
  }
  if (!read_optional_counter(reader, binding.path_authority_generation)) {
    return false;
  }
  if (!PolicyGeneration::read(reader, binding.policy_generation)) {
    return false;
  }
  if (!binding.is_valid()) {
    return false;
  }
  out = binding;
  return true;
}

const char* to_string(NextHopKind kind) noexcept {
  switch (kind) {
    case NextHopKind::DirectEndpoint:
      return "direct-endpoint";
    case NextHopKind::Router:
      return "router";
    case NextHopKind::Port:
      return "port";
    case NextHopKind::NextHopGroup:
      return "next-hop-group";
  }
  return "unknown";
}

bool parse_next_hop_kind(std::string_view text, NextHopKind& out) noexcept {
  return parse_enum_by_name(text, NextHopKind::DirectEndpoint, NextHopKind::NextHopGroup, to_string, out);
}

const char* to_string(BindingKind kind) noexcept {
  switch (kind) {
    case BindingKind::NextHop:
      return "next-hop";
    case BindingKind::AuthorizedPath:
      return "authorized-path";
  }
  return "unknown";
}

bool parse_binding_kind(std::string_view text, BindingKind& out) noexcept {
  return parse_enum_by_name(text, BindingKind::NextHop, BindingKind::AuthorizedPath, to_string, out);
}

const char* to_string(AppliedClassification classification) noexcept {
  switch (classification) {
    case AppliedClassification::Unknown:
      return "UNKNOWN";
    case AppliedClassification::Pending:
      return "PENDING";
    case AppliedClassification::Applied:
      return "APPLIED";
    case AppliedClassification::IdempotentAlreadyApplied:
      return "IDEMPOTENT_ALREADY_APPLIED";
    case AppliedClassification::Rejected:
      return "REJECTED";
    case AppliedClassification::NotSupported:
      return "NOT_SUPPORTED";
    case AppliedClassification::RetryableFailure:
      return "RETRYABLE_FAILURE";
    case AppliedClassification::PermanentFailure:
      return "PERMANENT_FAILURE";
    case AppliedClassification::Ambiguous:
      return "AMBIGUOUS";
    case AppliedClassification::Withdrawn:
      return "WITHDRAWN";
    case AppliedClassification::WithdrawFailed:
      return "WITHDRAW_FAILED";
    case AppliedClassification::BackendUnavailable:
      return "BACKEND_UNAVAILABLE";
  }
  return "UNKNOWN";
}

bool parse_applied_classification(std::string_view text, AppliedClassification& out) noexcept {
  return parse_enum_by_name(text, AppliedClassification::Unknown, AppliedClassification::BackendUnavailable,
                            to_string, out);
}

const char* to_string(ObservationClass classification) noexcept {
  switch (classification) {
    case ObservationClass::Unknown:
      return "UNKNOWN";
    case ObservationClass::Matched:
      return "MATCHED";
    case ObservationClass::Missing:
      return "MISSING";
    case ObservationClass::Diverged:
      return "DIVERGED";
    case ObservationClass::Extra:
      return "EXTRA";
    case ObservationClass::Unavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

bool parse_observation_class(std::string_view text, ObservationClass& out) noexcept {
  return parse_enum_by_name(text, ObservationClass::Unknown, ObservationClass::Unavailable, to_string, out);
}

const char* to_string(ProvenanceSourceClass source) noexcept {
  switch (source) {
    case ProvenanceSourceClass::Publisher:
      return "publisher";
    case ProvenanceSourceClass::CoordinatorRecovery:
      return "coordinator-recovery";
    case ProvenanceSourceClass::AdministrativeAction:
      return "administrative-action";
    case ProvenanceSourceClass::Reconciliation:
      return "reconciliation";
    case ProvenanceSourceClass::LineageMint:
      return "lineage-mint";
  }
  return "unknown";
}

bool parse_provenance_source(std::string_view text, ProvenanceSourceClass& out) noexcept {
  return parse_enum_by_name(text, ProvenanceSourceClass::Publisher, ProvenanceSourceClass::LineageMint, to_string, out);
}

const char* to_string(SupersessionKind kind) noexcept {
  switch (kind) {
    case SupersessionKind::None:
      return "none";
    case SupersessionKind::Replacement:
      return "replacement";
    case SupersessionKind::LineageTakeover:
      return "lineage-takeover";
  }
  return "unknown";
}

const char* to_string(RetirementCause cause) noexcept {
  switch (cause) {
    case RetirementCause::None:
      return "none";
    case RetirementCause::Administrative:
      return "administrative";
    case RetirementCause::Revoked:
      return "revoked";
    case RetirementCause::SupersededByLineage:
      return "superseded-by-lineage";
    case RetirementCause::EpochInvalidated:
      return "epoch-invalidated";
  }
  return "unknown";
}

bool NextHopBinding::is_valid() const noexcept {
  if (!entity_generation.is_valid()) {
    return false;
  }
  switch (kind) {
    case NextHopKind::NextHopGroup:
      return next_hop_group.is_valid() && !next_hop.is_valid();
    case NextHopKind::DirectEndpoint:
    case NextHopKind::Router:
    case NextHopKind::Port:
      return next_hop.is_valid() && !next_hop_group.is_valid();
  }
  return false;
}

bool RouteBinding::is_valid() const noexcept {
  switch (kind) {
    case BindingKind::AuthorizedPath:
      return path.is_valid() && path_authority_generation.is_valid();
    case BindingKind::NextHop:
      return next_hop.is_valid();
  }
  return false;
}

std::string RouteBinding::render() const {
  if (!is_valid()) {
    return "<invalid-binding>";
  }
  if (kind == BindingKind::AuthorizedPath) {
    return "path:" + path.render() + "@" + path_authority_generation.render();
  }
  if (next_hop.kind == NextHopKind::NextHopGroup) {
    return "next-hop-group:" + next_hop.next_hop_group.render() + "@" + next_hop.entity_generation.render();
  }
  return std::string("next-hop:") + to_string(next_hop.kind) + ":" + next_hop.next_hop.render() + "@" +
         next_hop.entity_generation.render();
}

Digest128 semantic_digest(const RouteRecord& record) {
  Fnv1a64 forward(0xCBF29CE484222325ull);
  Fnv1a64 backward(0x9AE16A3B2F90404Full);
  forward.update(std::string_view("routefabric.route-record.v1"));
  backward.update(std::string_view("routefabric.route-record.v1"));
  hash_record(record, forward, backward);
  return Digest128::from_u64_pair(mix64(forward.value()), mix64(backward.value()));
}

void write_route_record(ByteWriter& writer, const RouteRecord& record) {
  writer.u8(kRecordEncodingTag);
  record.id.write(writer);
  record.key.write(writer);
  record.generation.write(writer);
  record.authority_generation.write(writer);
  record.invalidation_watermark.write(writer);
  record.programming_generation.write(writer);
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  write_route_binding(writer, record.binding);
  write_applied(writer, record.applied);
  write_observation(writer, record.observation);
  write_provenance(writer, record.provenance);
  write_supersession(writer, record.supersession);
  write_retirement(writer, record.retirement);
  writer.u32(static_cast<std::uint32_t>(record.history.size()));
  for (const LineageEntry& entry : record.history) {
    write_history_entry(writer, entry);
  }
}

bool read_route_record(ByteReader& reader, const Limits& limits, RouteRecord& out, std::string& why) {
  std::uint8_t tag = 0;
  if (!reader.u8(tag) || tag != kRecordEncodingTag) {
    why = "unsupported route record encoding tag";
    return false;
  }
  RouteRecord record;
  if (!RouteId::read(reader, record.id)) {
    why = "malformed RouteId";
    return false;
  }
  if (!RouteKey::read(reader, limits.max_destination_chars, record.key)) {
    why = "malformed route key or destination";
    return false;
  }
  if (!RouteGeneration::read(reader, record.generation)) {
    why = "malformed or zero route generation";
    return false;
  }
  if (!RouteAuthorityGeneration::read(reader, record.authority_generation)) {
    why = "malformed or zero route authority generation";
    return false;
  }
  if (!RouteAuthorityGeneration::read(reader, record.invalidation_watermark)) {
    why = "malformed or zero invalidation watermark";
    return false;
  }
  if (!ProgrammingGeneration::read(reader, record.programming_generation)) {
    why = "malformed or zero programming generation";
    return false;
  }
  std::uint8_t lifecycle = 0;
  if (!reader.u8(lifecycle) || !is_lifecycle_value(lifecycle)) {
    why = "malformed lifecycle value";
    return false;
  }
  record.lifecycle = static_cast<RouteLifecycle>(lifecycle);
  if (!read_route_binding(reader, record.binding)) {
    why = "malformed route binding";
    return false;
  }
  if (!read_applied(reader, record.applied)) {
    why = "malformed applied state";
    return false;
  }
  if (!read_observation(reader, record.observation)) {
    why = "malformed backend observation";
    return false;
  }
  if (!read_provenance(reader, record.provenance)) {
    why = "malformed provenance";
    return false;
  }
  if (!read_supersession(reader, record.supersession)) {
    why = "malformed supersession record";
    return false;
  }
  if (!read_retirement(reader, record.retirement)) {
    why = "malformed retirement record";
    return false;
  }
  std::uint32_t history_count = 0;
  if (!reader.u32(history_count)) {
    why = "malformed history count";
    return false;
  }
  if (static_cast<std::size_t>(history_count) > limits.max_history_entries_per_route) {
    why = "history exceeds the configured bound";
    return false;
  }
  record.history.reserve(history_count);
  for (std::uint32_t i = 0; i < history_count; ++i) {
    LineageEntry entry;
    if (!read_history_entry(reader, entry)) {
      why = "malformed history entry";
      return false;
    }
    record.history.push_back(std::move(entry));
  }

  const Status status = validate_route_record(record, limits);
  if (!status) {
    why = status.error().detail();
    return false;
  }
  out = std::move(record);
  return true;
}

void write_revocation(ByteWriter& writer, const RevocationRecord& record) {
  record.route.write(writer);
  record.key.write(writer);
  record.generation.write(writer);
  record.epoch.write(writer);
  record.attempt.write(writer);
  writer.string(record.reason);
}

bool read_revocation(ByteReader& reader, const Limits& limits, RevocationRecord& out, std::string& why) {
  RevocationRecord record;
  if (!RouteId::read(reader, record.route)) {
    why = "malformed revocation RouteId";
    return false;
  }
  if (!RouteKey::read(reader, limits.max_destination_chars, record.key)) {
    why = "malformed revocation route key";
    return false;
  }
  if (!RouteGeneration::read(reader, record.generation)) {
    why = "malformed revocation generation";
    return false;
  }
  if (!CoordinatorEpoch::read(reader, record.epoch)) {
    why = "malformed revocation epoch";
    return false;
  }
  if (!MutationAttemptId::read(reader, record.attempt)) {
    why = "malformed revocation attempt identity";
    return false;
  }
  if (!reader.string(kMaxDiagnosticChars, record.reason)) {
    why = "malformed revocation reason";
    return false;
  }
  out = std::move(record);
  return true;
}

Status validate_route_record(const RouteRecord& record, const Limits& limits) {
  if (!record.id.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "route record carries a zero RouteId");
  }
  if (!record.key.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "route record carries an invalid route key");
  }
  if (!record.generation.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "route generation must be at least 1");
  }
  if (!record.authority_generation.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "route authority generation must be at least 1");
  }
  if (!record.invalidation_watermark.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "invalidation watermark must be at least 1");
  }
  if (!record.programming_generation.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "programming generation must be at least 1");
  }
  if (!is_lifecycle_value(static_cast<std::uint8_t>(record.lifecycle))) {
    return make_error(StatusCode::MalformedEncoding, "route record carries an unknown lifecycle value");
  }
  if (!record.binding.is_valid()) {
    return make_error(StatusCode::MalformedEncoding, "route record carries an invalid desired binding");
  }
  if (record.history.size() > limits.max_history_entries_per_route) {
    return make_error(StatusCode::LimitExceeded, "route history exceeds the configured bound");
  }
  if (record.applied.programmed_generation.is_valid() && record.applied.programmed_generation > record.generation) {
    return make_error(StatusCode::MalformedEncoding,
                      "applied state references a desired generation newer than the record");
  }
  if (record.applied.programming_generation.is_valid() &&
      record.applied.programming_generation > record.programming_generation) {
    return make_error(StatusCode::MalformedEncoding,
                      "applied state references a programming generation newer than the record watermark");
  }
  if (record.applied.detail.size() > kMaxDiagnosticChars || record.observation.detail.size() > kMaxDiagnosticChars ||
      record.supersession.reason.size() > kMaxDiagnosticChars ||
      record.retirement.reason.size() > kMaxDiagnosticChars) {
    return make_error(StatusCode::LimitExceeded, "diagnostic text exceeds the configured bound");
  }
  if (record.observation.observed_generation.is_valid() && record.observation.observed_generation > record.generation) {
    return make_error(StatusCode::MalformedEncoding,
                      "backend observation references a generation newer than the record");
  }
  if (record.invalidation_watermark > record.authority_generation) {
    return make_error(StatusCode::MalformedEncoding, "invalidation watermark exceeds the authority generation");
  }
  RouteGeneration previous;
  bool first = true;
  for (const LineageEntry& entry : record.history) {
    if (!entry.generation.is_valid() || entry.generation > record.generation) {
      return make_error(StatusCode::MalformedEncoding, "history entry references an impossible generation");
    }
    if (entry.prior_generation.is_valid() && entry.prior_generation >= entry.generation) {
      return make_error(StatusCode::MalformedEncoding, "history entry records a non-advancing prior generation");
    }
    if (!first && entry.generation < previous) {
      return make_error(StatusCode::MalformedEncoding, "history generations must not decrease");
    }
    previous = entry.generation;
    first = false;
  }
  if (record.lifecycle == RouteLifecycle::Retired) {
    if (record.retirement.cause == RetirementCause::None) {
      return make_error(StatusCode::MalformedEncoding, "a retired route must carry a retirement cause");
    }
    if (!record.retirement.generation.is_valid() || !record.retirement.epoch.is_valid() ||
        !record.retirement.attempt.is_valid()) {
      return make_error(StatusCode::MalformedEncoding, "a retired route must carry a complete retirement record");
    }
  } else if (record.retirement.cause != RetirementCause::None) {
    return make_error(StatusCode::MalformedEncoding, "a non-retired route must not carry a retirement cause");
  }
  if (record.lifecycle == RouteLifecycle::Superseded && record.supersession.kind == SupersessionKind::None) {
    return make_error(StatusCode::MalformedEncoding, "a superseded route must carry a supersession record");
  }
  if (record.lifecycle == RouteLifecycle::Installed && !record.applied.reports_applied()) {
    return make_error(StatusCode::MalformedEncoding, "an installed route must carry an applied backend outcome");
  }
  if (record.lifecycle == RouteLifecycle::Withdrawn && !record.applied.reports_not_present()) {
    return make_error(StatusCode::MalformedEncoding, "a withdrawn route must carry a withdrawn backend outcome");
  }
  if (record.lifecycle == RouteLifecycle::Failed) {
    const AppliedClassification classification = record.applied.classification;
    if (classification != AppliedClassification::Rejected && classification != AppliedClassification::NotSupported &&
        classification != AppliedClassification::PermanentFailure) {
      return make_error(StatusCode::MalformedEncoding, "a failed route must carry a definitive backend refusal");
    }
  }
  return ok_status();
}

}  // namespace routefabric
