#ifndef ROUTEFABRIC_PERSISTENCE_HPP
#define ROUTEFABRIC_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/error.hpp"
#include "routefabric/limits.hpp"
#include "routefabric/record.hpp"

namespace routefabric {

inline constexpr std::uint64_t kPersistenceMagic = 0x3152545342465452ull;
inline constexpr std::uint64_t kJournalEntryMagic = 0x31524A4652465452ull;
// Persistence frames use the same fixed header and trailer sizes as the wire
// format, but the two representations are versioned independently.
inline constexpr std::size_t kPersistFrameHeaderBytes = 16;
inline constexpr std::size_t kPersistFrameTrailerBytes = 12;

// Durability contract selected at runtime construction time.
//   None     - no store; mutations are acknowledged without persistence.
//   Snapshot - the whole state is atomically rewritten before every
//              acknowledgment (simple, correct, O(routes) per mutation).
//   Journal  - an integrity-checked append-only journal entry is flushed before
//              every acknowledgment, with periodic snapshot compaction.
enum class Durability : std::uint8_t {
  None = 0,
  Snapshot = 1,
  Journal = 2,
};

const char* to_string(Durability durability) noexcept;
bool parse_durability(std::string_view text, Durability& out) noexcept;

// Durable programming attempt state. Unresolved attempts surviving a restart are
// exactly the crash-ambiguity set: the runtime cannot prove whether the backend
// applied them.
struct ProgrammingAttemptRecord {
  ProgrammingAttemptId attempt;
  RouteId route;
  RouteGeneration desired_generation;
  ProgrammingGeneration programming_generation;
  ProgrammingOperation operation = ProgrammingOperation::Install;
  CoordinatorEpoch epoch;
  bool resolved = false;

  friend bool operator==(const ProgrammingAttemptRecord&, const ProgrammingAttemptRecord&) = default;
};

// Complete durable state of one Route Fabric coordinator.
struct PersistedState {
  CoordinatorEpoch epoch;
  PolicyGeneration policy_generation;
  std::vector<RouteRecord> routes;
  std::vector<RevocationRecord> revocations;
  std::vector<ProgrammingAttemptRecord> attempts;

  // Semantic digest over the durable state; independent of insertion order.
  Digest128 digest() const;
};

// One durable mutation batch.
struct JournalEntry {
  CoordinatorEpoch epoch;
  PolicyGeneration policy_generation;
  std::vector<RouteRecord> upserts;
  std::vector<RevocationRecord> revocations;
  std::vector<ProgrammingAttemptRecord> attempts;
};

// Builds a persistence frame (header, payload, integrity trailer) around an
// arbitrary payload. Exposed so that semantic validation can be tested
// independently of the integrity check.
std::vector<std::uint8_t> encode_persistence_frame(std::uint64_t magic, std::span<const std::uint8_t> payload);

void write_attempt_record(ByteWriter& writer, const ProgrammingAttemptRecord& record);
bool read_attempt_record(ByteReader& reader, ProgrammingAttemptRecord& out, std::string& why);

// Versioned, integrity-checked, atomically replaced persistence.
//
// Layout of both files: an explicit header (magic, format version, payload
// length), a deterministic payload, and an integrity trailer (CRC-32C plus a
// 64-bit content hash) covering the payload. Trailers are non-cryptographic:
// they detect corruption, they do not authenticate a writer.
class RouteStore {
 public:
  RouteStore(std::filesystem::path base_path, Limits limits, Durability durability);

  Durability durability() const noexcept { return durability_; }
  const Limits& limits() const noexcept { return limits_; }

  const std::filesystem::path& snapshot_path() const noexcept { return snapshot_path_; }
  const std::filesystem::path& journal_path() const noexcept { return journal_path_; }

  bool exists() const;

  // Reads the snapshot and replays the journal. A torn or corrupt journal tail
  // is discarded and the journal is truncated to the last valid entry; such a
  // tail can only originate from a mutation that was never acknowledged.
  Expected<PersistedState> Load();

  // Atomically replaces the snapshot and clears the journal.
  Status SaveSnapshot(const PersistedState& state);

  // Appends one journal entry and flushes it to stable storage.
  Status AppendJournal(const JournalEntry& entry);

  std::size_t journal_entry_count() const noexcept { return journal_entries_; }
  std::uint64_t journal_byte_count() const noexcept { return journal_bytes_; }

  // Compaction threshold, expressed in journal entries.
  std::size_t compaction_threshold = 1024;

  bool should_compact() const noexcept {
    return durability_ == Durability::Journal &&
           (journal_entries_ >= compaction_threshold || journal_bytes_ >= (limits_.max_persist_bytes / 4));
  }

  // Removes both files (used by tests and by explicit operator cleanup).
  Status Remove();

  // Codecs are public so that corruption and truncation can be tested directly.
  static std::vector<std::uint8_t> EncodeSnapshot(const PersistedState& state, const Limits& limits);
  static bool DecodeSnapshot(std::span<const std::uint8_t> image, const Limits& limits, PersistedState& out,
                             std::string& why);
  static std::vector<std::uint8_t> EncodeJournalEntry(const JournalEntry& entry, const Limits& limits);
  static bool DecodeJournalEntry(std::span<const std::uint8_t> image, const Limits& limits, JournalEntry& out,
                                 std::string& why);

 private:
  std::filesystem::path snapshot_path_;
  std::filesystem::path journal_path_;
  Limits limits_;
  Durability durability_ = Durability::None;
  std::size_t journal_entries_ = 0;
  std::uint64_t journal_bytes_ = 0;
};

// Portable file helpers, exposed for the integrity and crash tests.
bool write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& why);
bool append_file_flushed(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& why);
bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& why);
bool truncate_file(const std::filesystem::path& path, std::uint64_t size, std::string& why);

}  // namespace routefabric

#endif  // ROUTEFABRIC_PERSISTENCE_HPP
