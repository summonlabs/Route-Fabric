#include "routefabric/persistence.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "routefabric/hash.hpp"
#include "routefabric/version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace routefabric {
namespace {

bool same_magic(const std::uint8_t* data, std::uint64_t magic) {
  return read_le64(data) == magic;
}

void write_frame(ByteWriter& writer, std::uint64_t magic, std::span<const std::uint8_t> payload) {
  writer.u64(magic);
  writer.u32(kPersistenceFormatVersion);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.raw(payload);
  writer.u32(Crc32c::compute(payload));
  Fnv1a64 fnv;
  fnv.update(payload);
  writer.u64(fnv.value());
}

// Validates a frame and returns the payload span. Rejects wrong magic,
// unsupported version, oversized payloads, truncation, trailing bytes and
// integrity failures.
bool read_frame(std::span<const std::uint8_t> image, std::uint64_t expected_magic, const Limits& limits,
                std::span<const std::uint8_t>& payload, std::string& why) {
  if (image.size() < kPersistFrameHeaderBytes + kPersistFrameTrailerBytes) {
    why = "truncated header";
    return false;
  }
  if (!same_magic(image.data(), expected_magic)) {
    why = "wrong magic";
    return false;
  }
  const std::uint32_t version = read_le32(image.data() + 8);
  if (version != kPersistenceFormatVersion) {
    why = "unsupported format version";
    return false;
  }
  const std::uint32_t payload_length = read_le32(image.data() + 12);
  if (static_cast<std::size_t>(payload_length) > limits.max_persist_bytes) {
    why = "payload exceeds the configured bound";
    return false;
  }
  const std::size_t expected_size = kPersistFrameHeaderBytes + static_cast<std::size_t>(payload_length) + kPersistFrameTrailerBytes;
  if (image.size() < expected_size) {
    why = "truncated payload";
    return false;
  }
  if (image.size() > expected_size) {
    why = "trailing bytes after the integrity trailer";
    return false;
  }
  payload = image.subspan(kPersistFrameHeaderBytes, payload_length);
  const std::uint32_t stored_crc = read_le32(image.data() + kPersistFrameHeaderBytes + payload_length);
  if (stored_crc != Crc32c::compute(payload)) {
    why = "integrity trailer mismatch";
    return false;
  }
  Fnv1a64 fnv;
  fnv.update(payload);
  if (read_le64(image.data() + kPersistFrameHeaderBytes + payload_length + 4) != fnv.value()) {
    why = "content hash mismatch";
    return false;
  }
  return true;
}

template <typename Id>
bool read_required_counter(ByteReader& reader, Id& out) {
  return Id::read(reader, out);
}

void write_attempt(ByteWriter& writer, const ProgrammingAttemptRecord& record) {
  record.attempt.write(writer);
  record.route.write(writer);
  record.desired_generation.write(writer);
  record.programming_generation.write(writer);
  writer.u8(static_cast<std::uint8_t>(record.operation));
  record.epoch.write(writer);
  writer.boolean(record.resolved);
}

bool read_attempt(ByteReader& reader, ProgrammingAttemptRecord& out) {
  ProgrammingAttemptRecord record;
  if (!ProgrammingAttemptId::read(reader, record.attempt)) {
    return false;
  }
  if (!RouteId::read(reader, record.route)) {
    return false;
  }
  if (!RouteGeneration::read(reader, record.desired_generation)) {
    return false;
  }
  if (!ProgrammingGeneration::read(reader, record.programming_generation)) {
    return false;
  }
  std::uint8_t operation = 0;
  if (!reader.u8(operation)) {
    return false;
  }
  if (operation < static_cast<std::uint8_t>(ProgrammingOperation::Install) ||
      operation > static_cast<std::uint8_t>(ProgrammingOperation::Withdraw)) {
    return false;
  }
  record.operation = static_cast<ProgrammingOperation>(operation);
  if (!CoordinatorEpoch::read(reader, record.epoch)) {
    return false;
  }
  if (!reader.boolean(record.resolved)) {
    return false;
  }
  out = std::move(record);
  return true;
}

void write_state_body(ByteWriter& writer, const std::vector<RouteRecord>& routes,
                      const std::vector<RevocationRecord>& revocations,
                      const std::vector<ProgrammingAttemptRecord>& attempts) {
  writer.u32(static_cast<std::uint32_t>(routes.size()));
  for (const RouteRecord& record : routes) {
    write_route_record(writer, record);
  }
  writer.u32(static_cast<std::uint32_t>(revocations.size()));
  for (const RevocationRecord& revocation : revocations) {
    write_revocation(writer, revocation);
  }
  writer.u32(static_cast<std::uint32_t>(attempts.size()));
  for (const ProgrammingAttemptRecord& attempt : attempts) {
    write_attempt(writer, attempt);
  }
}

bool read_state_body(ByteReader& reader, const Limits& limits, std::vector<RouteRecord>& routes,
                     std::vector<RevocationRecord>& revocations, std::vector<ProgrammingAttemptRecord>& attempts,
                     std::string& why) {
  std::uint32_t route_count = 0;
  if (!reader.u32(route_count)) {
    why = "malformed route count";
    return false;
  }
  if (static_cast<std::size_t>(route_count) > limits.max_routes) {
    why = "route count exceeds the configured bound";
    return false;
  }
  routes.reserve(route_count);
  for (std::uint32_t i = 0; i < route_count; ++i) {
    RouteRecord record;
    if (!read_route_record(reader, limits, record, why)) {
      return false;
    }
    routes.push_back(std::move(record));
  }
  std::uint32_t revocation_count = 0;
  if (!reader.u32(revocation_count)) {
    why = "malformed revocation count";
    return false;
  }
  if (static_cast<std::size_t>(revocation_count) > limits.max_routes) {
    why = "revocation count exceeds the configured bound";
    return false;
  }
  revocations.reserve(revocation_count);
  for (std::uint32_t i = 0; i < revocation_count; ++i) {
    RevocationRecord revocation;
    if (!read_revocation(reader, limits, revocation, why)) {
      return false;
    }
    revocations.push_back(std::move(revocation));
  }
  std::uint32_t attempt_count = 0;
  if (!reader.u32(attempt_count)) {
    why = "malformed attempt count";
    return false;
  }
  if (static_cast<std::size_t>(attempt_count) > limits.max_outstanding_programming) {
    why = "attempt count exceeds the configured bound";
    return false;
  }
  attempts.reserve(attempt_count);
  for (std::uint32_t i = 0; i < attempt_count; ++i) {
    ProgrammingAttemptRecord attempt;
    if (!read_attempt(reader, attempt)) {
      why = "malformed programming attempt record";
      return false;
    }
    attempts.push_back(std::move(attempt));
  }
  return true;
}

}  // namespace

const char* to_string(Durability durability) noexcept {
  switch (durability) {
    case Durability::None:
      return "none";
    case Durability::Snapshot:
      return "snapshot";
    case Durability::Journal:
      return "journal";
  }
  return "unknown";
}

bool parse_durability(std::string_view text, Durability& out) noexcept {
  if (text == "none") {
    out = Durability::None;
    return true;
  }
  if (text == "snapshot") {
    out = Durability::Snapshot;
    return true;
  }
  if (text == "journal") {
    out = Durability::Journal;
    return true;
  }
  return false;
}

void write_attempt_record(ByteWriter& writer, const ProgrammingAttemptRecord& record) { write_attempt(writer, record); }

bool read_attempt_record(ByteReader& reader, ProgrammingAttemptRecord& out, std::string& why) {
  if (!read_attempt(reader, out)) {
    why = "malformed programming attempt record";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_persistence_frame(std::uint64_t magic, std::span<const std::uint8_t> payload) {
  ByteWriter writer;
  write_frame(writer, magic, payload);
  return writer.take();
}

Digest128 PersistedState::digest() const {
  std::vector<Digest128> digests;
  digests.reserve(routes.size());
  for (const RouteRecord& record : routes) {
    digests.push_back(semantic_digest(record));
  }
  std::sort(digests.begin(), digests.end());
  Fnv1a64 forward(0xCBF29CE484222325ull);
  Fnv1a64 backward(0x9AE16A3B2F90404Full);
  forward.update(std::string_view("routefabric.persisted-state.v1"));
  backward.update(std::string_view("routefabric.persisted-state.v1"));
  for (const Digest128& digest : digests) {
    forward.update(digest.bytes());
    backward.update(digest.bytes());
  }
  for (const RevocationRecord& revocation : revocations) {
    forward.update(revocation.route.bytes());
    backward.update(revocation.route.bytes());
    const std::uint64_t generation = revocation.generation.value();
    ByteWriter scratch;
    scratch.u64(generation);
    forward.update(scratch.buffer());
    backward.update(scratch.buffer());
  }
  const std::uint8_t attempt_count = static_cast<std::uint8_t>(std::min<std::size_t>(attempts.size(), 255));
  forward.update_byte(attempt_count);
  backward.update_byte(attempt_count);
  return Digest128::from_u64_pair(mix64(forward.value()), mix64(backward.value()));
}

std::vector<std::uint8_t> RouteStore::EncodeSnapshot(const PersistedState& state, const Limits& limits) {
  ByteWriter payload;
  payload.u64(state.epoch.is_valid() ? state.epoch.value() : 1);
  payload.u64(state.policy_generation.is_valid() ? state.policy_generation.value() : 1);
  write_state_body(payload, state.routes, state.revocations, state.attempts);
  const Digest128 digest = state.digest();
  payload.raw(digest.bytes());

  if (payload.size() > limits.max_persist_bytes) {
    return {};
  }
  ByteWriter writer;
  write_frame(writer, kPersistenceMagic, payload.buffer());
  return writer.take();
}

bool RouteStore::DecodeSnapshot(std::span<const std::uint8_t> image, const Limits& limits, PersistedState& out,
                                std::string& why) {
  std::span<const std::uint8_t> payload;
  if (!read_frame(image, kPersistenceMagic, limits, payload, why)) {
    return false;
  }
  ByteReader reader(payload);
  std::uint64_t epoch = 0;
  std::uint64_t policy_generation = 0;
  if (!reader.u64(epoch) || !reader.u64(policy_generation)) {
    why = "malformed epoch header";
    return false;
  }
  if (epoch == 0 || epoch > CoordinatorEpoch::kMaxValue) {
    why = "impossible coordinator epoch";
    return false;
  }
  if (policy_generation == 0 || policy_generation > PolicyGeneration::kMaxValue) {
    why = "impossible policy generation";
    return false;
  }
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(epoch);
  state.policy_generation = PolicyGeneration::from_value(policy_generation);
  if (!read_state_body(reader, limits, state.routes, state.revocations, state.attempts, why)) {
    return false;
  }
  std::span<const std::uint8_t> stored_digest;
  if (!reader.raw(16, stored_digest)) {
    why = "missing state digest";
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes inside the snapshot payload";
    return false;
  }
  Digest128 digest;
  std::array<std::uint8_t, 16> digest_bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    digest_bytes[i] = stored_digest[i];
  }
  digest = Digest128(digest_bytes);
  if (!(digest == state.digest())) {
    why = "state digest mismatch";
    return false;
  }

  // Duplicate detection: two records may never claim the same RouteId or the
  // same semantic route key.
  std::vector<RouteId> ids;
  ids.reserve(state.routes.size());
  for (const RouteRecord& record : state.routes) {
    ids.push_back(record.id);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
    why = "duplicate RouteId in the stored state";
    return false;
  }
  std::vector<RouteKey> keys;
  keys.reserve(state.routes.size());
  for (const RouteRecord& record : state.routes) {
    keys.push_back(record.key);
  }
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
    why = "duplicate route key in the stored state";
    return false;
  }
  std::vector<ProgrammingAttemptId> attempts;
  attempts.reserve(state.attempts.size());
  for (const ProgrammingAttemptRecord& record : state.attempts) {
    attempts.push_back(record.attempt);
  }
  std::sort(attempts.begin(), attempts.end());
  if (std::adjacent_find(attempts.begin(), attempts.end()) != attempts.end()) {
    why = "duplicate programming attempt identity in the stored state";
    return false;
  }
  std::vector<RouteId> revoked;
  revoked.reserve(state.revocations.size());
  for (const RevocationRecord& record : state.revocations) {
    revoked.push_back(record.route);
  }
  std::sort(revoked.begin(), revoked.end());
  if (std::adjacent_find(revoked.begin(), revoked.end()) != revoked.end()) {
    why = "duplicate revocation record in the stored state";
    return false;
  }
  out = std::move(state);
  return true;
}

std::vector<std::uint8_t> RouteStore::EncodeJournalEntry(const JournalEntry& entry, const Limits& limits) {
  ByteWriter payload;
  payload.u64(entry.epoch.is_valid() ? entry.epoch.value() : 1);
  payload.u64(entry.policy_generation.is_valid() ? entry.policy_generation.value() : 1);
  write_state_body(payload, entry.upserts, entry.revocations, entry.attempts);
  if (payload.size() > limits.max_persist_bytes) {
    return {};
  }
  ByteWriter writer;
  write_frame(writer, kJournalEntryMagic, payload.buffer());
  return writer.take();
}

bool RouteStore::DecodeJournalEntry(std::span<const std::uint8_t> image, const Limits& limits, JournalEntry& out,
                                    std::string& why) {
  std::span<const std::uint8_t> payload;
  if (!read_frame(image, kJournalEntryMagic, limits, payload, why)) {
    return false;
  }
  ByteReader reader(payload);
  std::uint64_t epoch = 0;
  std::uint64_t policy_generation = 0;
  if (!reader.u64(epoch) || !reader.u64(policy_generation)) {
    why = "malformed journal header";
    return false;
  }
  if (epoch == 0 || epoch > CoordinatorEpoch::kMaxValue) {
    why = "impossible coordinator epoch";
    return false;
  }
  if (policy_generation == 0 || policy_generation > PolicyGeneration::kMaxValue) {
    why = "impossible policy generation";
    return false;
  }
  JournalEntry entry;
  entry.epoch = CoordinatorEpoch::from_value(epoch);
  entry.policy_generation = PolicyGeneration::from_value(policy_generation);
  if (!read_state_body(reader, limits, entry.upserts, entry.revocations, entry.attempts, why)) {
    return false;
  }
  if (!reader.at_end()) {
    why = "trailing bytes inside the journal payload";
    return false;
  }
  out = std::move(entry);
  return true;
}

RouteStore::RouteStore(std::filesystem::path base_path, Limits limits, Durability durability)
    : snapshot_path_(base_path.string() + ".snapshot"),
      journal_path_(base_path.string() + ".journal"),
      limits_(limits),
      durability_(durability) {}

bool RouteStore::exists() const {
  std::error_code error;
  return std::filesystem::exists(snapshot_path_, error) || std::filesystem::exists(journal_path_, error);
}

Status RouteStore::Remove() {
  std::error_code error;
  std::filesystem::remove(snapshot_path_, error);
  error.clear();
  std::filesystem::remove(journal_path_, error);
  journal_entries_ = 0;
  journal_bytes_ = 0;
  return ok_status();
}

Expected<PersistedState> RouteStore::Load() {
  PersistedState state;
  state.epoch = CoordinatorEpoch::from_value(1);
  state.policy_generation = PolicyGeneration::from_value(1);

  std::error_code error;
  if (std::filesystem::exists(snapshot_path_, error)) {
    std::vector<std::uint8_t> image;
    std::string why;
    if (!read_file_bytes(snapshot_path_, image, why)) {
      return make_error<PersistedState>(StatusCode::PersistenceFailure, "cannot read store snapshot: " + why);
    }
    std::string decode_why;
    if (!DecodeSnapshot(image, limits_, state, decode_why)) {
      return make_error<PersistedState>(StatusCode::PersistenceFailure, "corrupt store snapshot: " + decode_why);
    }
  }

  journal_entries_ = 0;
  journal_bytes_ = 0;
  error.clear();
  if (!std::filesystem::exists(journal_path_, error)) {
    return state;
  }
  std::vector<std::uint8_t> journal;
  std::string why;
  if (!read_file_bytes(journal_path_, journal, why)) {
    return make_error<PersistedState>(StatusCode::PersistenceFailure, "cannot read store journal: " + why);
  }

  std::size_t offset = 0;
  std::size_t valid_bytes = 0;
  while (offset + kPersistFrameHeaderBytes + kPersistFrameTrailerBytes <= journal.size()) {
    const std::span<const std::uint8_t> remaining(journal.data() + offset, journal.size() - offset);
    std::uint32_t payload_length = 0;
    if (remaining.size() < kPersistFrameHeaderBytes) {
      break;
    }
    if (read_le64(remaining.data()) != kJournalEntryMagic) {
      break;
    }
    payload_length = read_le32(remaining.data() + 12);
    if (static_cast<std::size_t>(payload_length) > limits_.max_persist_bytes) {
      break;
    }
    const std::size_t frame_size = kPersistFrameHeaderBytes + static_cast<std::size_t>(payload_length) + kPersistFrameTrailerBytes;
    if (offset + frame_size > journal.size()) {
      break;  // torn tail: never acknowledged
    }
    JournalEntry entry;
    std::string entry_why;
    if (!DecodeJournalEntry(remaining.first(frame_size), limits_, entry, entry_why)) {
      break;
    }
    // Replay.
    if (entry.epoch > state.epoch) {
      state.epoch = entry.epoch;
    }
    if (entry.policy_generation > state.policy_generation) {
      state.policy_generation = entry.policy_generation;
    }
    for (const RouteRecord& record : entry.upserts) {
      const auto existing = std::find_if(state.routes.begin(), state.routes.end(),
                                         [&record](const RouteRecord& candidate) { return candidate.id == record.id; });
      if (existing == state.routes.end()) {
        state.routes.push_back(record);
      } else if (record.generation > existing->generation) {
        *existing = record;
      } else if (record.generation == existing->generation) {
        *existing = record;  // idempotent replay of the same generation
      }
    }
    for (const RevocationRecord& revocation : entry.revocations) {
      const auto existing = std::find_if(state.revocations.begin(), state.revocations.end(),
                                         [&revocation](const RevocationRecord& candidate) {
                                           return candidate.route == revocation.route;
                                         });
      if (existing == state.revocations.end()) {
        state.revocations.push_back(revocation);
      } else {
        *existing = revocation;
      }
    }
    for (const ProgrammingAttemptRecord& attempt : entry.attempts) {
      const auto existing = std::find_if(state.attempts.begin(), state.attempts.end(),
                                         [&attempt](const ProgrammingAttemptRecord& candidate) {
                                           return candidate.attempt == attempt.attempt;
                                         });
      if (existing == state.attempts.end()) {
        state.attempts.push_back(attempt);
      } else {
        *existing = attempt;
      }
    }
    offset += frame_size;
    valid_bytes = offset;
    ++journal_entries_;
  }
  journal_bytes_ = valid_bytes;

  if (valid_bytes != journal.size()) {
    // Discard the torn tail so that the next append cannot extend a corrupt
    // journal. A torn tail can only come from a mutation that was never
    // acknowledged.
    std::string truncate_why;
    if (!truncate_file(journal_path_, valid_bytes, truncate_why)) {
      return make_error<PersistedState>(StatusCode::PersistenceFailure, "cannot truncate torn journal tail: " + truncate_why);
    }
  }
  return state;
}

Status RouteStore::SaveSnapshot(const PersistedState& state) {
  if (durability_ == Durability::None) {
    return make_error(StatusCode::Unsupported, "the store is configured without durability");
  }
  const std::vector<std::uint8_t> image = EncodeSnapshot(state, limits_);
  if (image.empty()) {
    return make_error(StatusCode::LimitExceeded, "encoded snapshot exceeds the configured bound");
  }
  std::string why;
  if (!write_file_atomic(snapshot_path_, image, why)) {
    return make_error(StatusCode::PersistenceFailure, "cannot write store snapshot: " + why);
  }
  std::error_code error;
  std::filesystem::remove(journal_path_, error);
  journal_entries_ = 0;
  journal_bytes_ = 0;
  return ok_status();
}

Status RouteStore::AppendJournal(const JournalEntry& entry) {
  if (durability_ == Durability::None) {
    return make_error(StatusCode::Unsupported, "the store is configured without durability");
  }
  const std::vector<std::uint8_t> image = EncodeJournalEntry(entry, limits_);
  if (image.empty()) {
    return make_error(StatusCode::LimitExceeded, "encoded journal entry exceeds the configured bound");
  }
  std::string why;
  if (!append_file_flushed(journal_path_, image, why)) {
    return make_error(StatusCode::PersistenceFailure, "cannot append store journal: " + why);
  }
  ++journal_entries_;
  journal_bytes_ += image.size();
  return ok_status();
}

// ---------------------------------------------------------------------------
// Portable file helpers
// ---------------------------------------------------------------------------

bool write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& why) {
  const std::filesystem::path temporary = std::filesystem::path(path.string() + ".tmp");
  std::error_code error;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      why = "cannot create directory: " + error.message();
      return false;
    }
  }
#ifdef _WIN32
  const std::wstring temporary_path = temporary.wstring();
  HANDLE handle = ::CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    why = "cannot create temporary store file";
    return false;
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 20));
    DWORD chunk_written = 0;
    if (::WriteFile(handle, bytes.data() + written, chunk, &chunk_written, nullptr) == 0 ||
        chunk_written != chunk) {
      ::CloseHandle(handle);
      ::DeleteFileW(temporary_path.c_str());
      why = "short write to temporary store file";
      return false;
    }
    written += chunk_written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    ::CloseHandle(handle);
    ::DeleteFileW(temporary_path.c_str());
    why = "cannot flush temporary store file";
    return false;
  }
  ::CloseHandle(handle);
  if (::MoveFileExW(temporary_path.c_str(), path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    ::DeleteFileW(temporary_path.c_str());
    why = "cannot atomically replace the store file";
    return false;
  }
  return true;
#else
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      why = "cannot create temporary store file";
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      why = "short write to temporary store file";
      return false;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    why = "cannot atomically replace the store file: " + error.message();
    return false;
  }
  return true;
#endif
}

bool append_file_flushed(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& why) {
  std::error_code error;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      why = "cannot create directory: " + error.message();
      return false;
    }
  }
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    why = "cannot open the store journal for append";
    return false;
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 20));
    DWORD chunk_written = 0;
    if (::WriteFile(handle, bytes.data() + written, chunk, &chunk_written, nullptr) == 0 ||
        chunk_written != chunk) {
      ::CloseHandle(handle);
      why = "short write to the store journal";
      return false;
    }
    written += chunk_written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    ::CloseHandle(handle);
    why = "cannot flush the store journal";
    return false;
  }
  ::CloseHandle(handle);
  return true;
#else
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    why = "cannot open the store journal for append";
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  if (!out) {
    why = "short write to the store journal";
    return false;
  }
  return true;
#endif
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& why) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    why = "cannot open file for reading";
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    why = "cannot determine file size";
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) {
      why = "short read";
      return false;
    }
  }
  return true;
}

bool truncate_file(const std::filesystem::path& path, std::uint64_t size, std::string& why) {
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    why = "cannot open the store journal for truncation";
    return false;
  }
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0) {
    ::CloseHandle(handle);
    why = "cannot seek the store journal";
    return false;
  }
  if (::SetEndOfFile(handle) == 0) {
    ::CloseHandle(handle);
    why = "cannot truncate the store journal";
    return false;
  }
  ::FlushFileBuffers(handle);
  ::CloseHandle(handle);
  return true;
#else
  std::error_code error;
  std::filesystem::resize_file(path, size, error);
  if (error) {
    why = "cannot truncate the store journal: " + error.message();
    return false;
  }
  return true;
#endif
}

}  // namespace routefabric
