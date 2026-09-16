#include "test_framework.hpp"

#include <set>
#include <vector>

#include "harness.hpp"
#include "routefabric/hash.hpp"
#include "routefabric/ids.hpp"

using namespace routefabric;
using rftest::hex_id;

RF_TEST(route_id_rejects_malformed_encoding) {
  RF_CHECK(!RouteId::parse("").has_value());
  RF_CHECK(!RouteId::parse("00").has_value());
  RF_CHECK(!RouteId::parse(std::string(31, 'a')).has_value());
  RF_CHECK(!RouteId::parse(std::string(33, 'a')).has_value());
  RF_CHECK(!RouteId::parse("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz").has_value());
  RF_CHECK(!RouteId::parse("00000000000000000000000000000000").has_value());  // zero is never an identity
  RF_CHECK(!RouteId::parse("0000000000000000000000000000000g").has_value());
  RF_CHECK(RouteId::parse("00000000000000000000000000000001").has_value());
  RF_CHECK(RouteId::parse("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF").has_value());
}

RF_TEST(route_id_renders_canonically) {
  const Expected<RouteId> id = RouteId::parse("0123456789ABCDEF0123456789abcdef");
  RF_REQUIRE(id.has_value());
  RF_CHECK_EQ(id.value().render(), std::string("0123456789abcdef0123456789abcdef"));
  const Expected<RouteId> round_trip = RouteId::parse(id.value().render());
  RF_REQUIRE(round_trip.has_value());
  RF_CHECK(round_trip.value() == id.value());
  RF_CHECK_EQ(std::string(RouteId::kName), std::string("RouteId"));
}

RF_TEST(identities_are_not_interchangeable) {
  // The type system keeps identities apart; this test documents that two
  // different identity domains with the same bytes compare unequal after being
  // rendered, and that each carries its own domain name.
  const RouteId route = RouteId::parse("00000000000000000000000000000007").value();
  const PathId path = PathId::parse("00000000000000000000000000000007").value();
  RF_CHECK_EQ(route.render(), path.render());
  RF_CHECK_EQ(std::string(RouteId::kName), std::string("RouteId"));
  RF_CHECK_EQ(std::string(PathId::kName), std::string("PathId"));
  static_assert(!std::is_same_v<RouteId, PathId>);
}

RF_TEST(identity_ordering_is_deterministic) {
  std::vector<RouteId> ids;
  for (std::uint64_t value : {5ull, 1ull, 9ull, 3ull, 7ull}) {
    ids.push_back(RouteId::parse(hex_id(value)).value());
  }
  std::sort(ids.begin(), ids.end());
  RF_CHECK_EQ(ids.front().render(), hex_id(1));
  RF_CHECK_EQ(ids.back().render(), hex_id(9));
  std::set<RouteId> unique(ids.begin(), ids.end());
  RF_CHECK_EQ(unique.size(), static_cast<std::size_t>(5));
}

RF_TEST(token_identity_validation) {
  RF_CHECK(PublisherId::parse("publisher-a").has_value());
  RF_CHECK(PublisherId::parse("A1._-b").has_value());
  RF_CHECK(!PublisherId::parse("").has_value());
  RF_CHECK(!PublisherId::parse("-leading").has_value());
  RF_CHECK(!PublisherId::parse("has space").has_value());
  RF_CHECK(!PublisherId::parse("has/slash").has_value());
  RF_CHECK(!PublisherId::parse(std::string(65, 'a')).has_value());
  RF_CHECK(PublisherId::parse(std::string(64, 'a')).has_value());
  const Expected<PublisherId> id = PublisherId::parse("publisher-a");
  RF_REQUIRE(id.has_value());
  RF_CHECK_EQ(id.value().render(), std::string("publisher-a"));
}

RF_TEST(counter_identity_is_checked_and_monotonic) {
  const Expected<RouteGeneration> first = RouteGeneration::parse("1");
  RF_REQUIRE(first.has_value());
  RF_CHECK(!RouteGeneration::parse("0").has_value());
  RF_CHECK(!RouteGeneration::parse("-1").has_value());
  RF_CHECK(!RouteGeneration::parse("01").has_value());
  RF_CHECK(!RouteGeneration::parse(" 1").has_value());
  RF_CHECK(!RouteGeneration::parse("1 ").has_value());
  RF_CHECK(!RouteGeneration::parse("18446744073709551616").has_value());
  const Expected<RouteGeneration> second = first.value().next();
  RF_REQUIRE(second.has_value());
  RF_CHECK_EQ(second.value().value(), static_cast<std::uint64_t>(2));
  RF_CHECK(second.value() > first.value());

  // The counter never wraps: the last representable value has no successor.
  const RouteGeneration last = RouteGeneration::from_value(RouteGeneration::kMaxValue);
  RF_CHECK(!last.next().has_value());
}

RF_TEST(counter_identity_decoding_rejects_zero_and_overflow) {
  const std::uint8_t zero_encoded[8] = {};
  ByteReader reader(std::span<const std::uint8_t>(zero_encoded, 8));
  RouteGeneration generation;
  RF_CHECK(!RouteGeneration::read(reader, generation));

  ByteWriter writer;
  writer.u64(RouteGeneration::kMaxValue + 1);
  ByteReader overflow(writer.buffer());
  RF_CHECK(!RouteGeneration::read(overflow, generation));
}

RF_TEST(identity_encoding_round_trips) {
  ByteWriter writer;
  const RouteId route = RouteId::parse(hex_id(42)).value();
  const PublisherId publisher = PublisherId::parse("fabric-publisher").value();
  const PathAuthorityGeneration generation = PathAuthorityGeneration::parse("17").value();
  route.write(writer);
  publisher.write(writer);
  generation.write(writer);

  ByteReader reader(writer.buffer());
  RouteId read_route;
  PublisherId read_publisher;
  PathAuthorityGeneration read_generation;
  RF_CHECK(RouteId::read(reader, read_route));
  RF_CHECK(PublisherId::read(reader, read_publisher));
  RF_CHECK(PathAuthorityGeneration::read(reader, read_generation));
  RF_CHECK(reader.at_end());
  RF_CHECK(read_route == route);
  RF_CHECK(read_publisher == publisher);
  RF_CHECK(read_generation == generation);
}

RF_TEST(identity_decoding_rejects_zero_and_trailing_bytes) {
  ByteWriter writer;
  writer.raw(std::string(16, '\0'));
  ByteReader reader(writer.buffer());
  RouteId route;
  RF_CHECK(!RouteId::read(reader, route));

  ByteWriter token_writer;
  token_writer.string("publisher-a");
  token_writer.u8(0xAB);
  ByteReader token_reader(token_writer.buffer());
  PublisherId publisher;
  RF_CHECK(PublisherId::read(token_reader, publisher));
  RF_CHECK(!token_reader.at_end());
}

RF_TEST(seeded_id_source_is_reproducible_and_never_zero) {
  SeededIdSource first(1234);
  SeededIdSource second(1234);
  std::set<std::string> seen;
  for (int index = 0; index < 256; ++index) {
    const WorkerBootId left = generate_id<WorkerBootId>(first);
    const WorkerBootId right = generate_id<WorkerBootId>(second);
    RF_CHECK(left == right);
    RF_CHECK(left.is_valid());
    seen.insert(left.render());
  }
  RF_CHECK_EQ(seen.size(), static_cast<std::size_t>(256));

  SeededIdSource different(1235);
  SeededIdSource repeated(1234);
  RF_CHECK(!(generate_id<WorkerBootId>(different) == generate_id<WorkerBootId>(repeated)));
}

RF_TEST(digest_is_deterministic_and_domain_separated) {
  const Digest128 first = digest128("domain-a", std::string_view("payload"));
  const Digest128 second = digest128("domain-a", std::string_view("payload"));
  const Digest128 other_domain = digest128("domain-b", std::string_view("payload"));
  const Digest128 other_payload = digest128("domain-a", std::string_view("payload2"));
  RF_CHECK(first == second);
  RF_CHECK(!(first == other_domain));
  RF_CHECK(!(first == other_payload));
  const Expected<Digest128> parsed = Digest128::parse(first.to_hex());
  RF_REQUIRE(parsed.has_value());
  RF_CHECK(parsed.value() == first);
  RF_CHECK(!Digest128::parse(std::string(32, '0')).has_value());
  RF_CHECK(!Digest128::parse("short").has_value());
}

RF_TEST(crc32c_matches_known_vectors) {
  // CRC-32C of "123456789" is 0xE3069283.
  const std::string_view input = "123456789";
  const std::uint32_t value = Crc32c::compute(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
  RF_CHECK_EQ(value, 0xE3069283u);
  RF_CHECK_EQ(Crc32c::compute(std::span<const std::uint8_t>()), 0u);
}
