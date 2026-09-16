#include "test_framework.hpp"

#include "harness.hpp"
#include "routefabric/destination.hpp"

using namespace routefabric;

RF_TEST(ipv4_prefixes_canonicalize_host_bits) {
  const Expected<Destination> masked = Destination::parse_ipv4_prefix("10.0.0.1/24");
  RF_REQUIRE(masked.has_value());
  RF_CHECK_EQ(masked.value().render(), std::string("10.0.0.0/24"));
  RF_CHECK_EQ(masked.value().kind(), DestinationKind::Ipv4Prefix);
  RF_CHECK_EQ(static_cast<int>(masked.value().prefix_length()), 24);

  const Expected<Destination> host = Destination::parse_ipv4_prefix("192.168.1.7/32");
  RF_REQUIRE(host.has_value());
  RF_CHECK_EQ(host.value().render(), std::string("192.168.1.7/32"));

  const Expected<Destination> default_route = Destination::parse_ipv4_prefix("0.0.0.0/0");
  RF_REQUIRE(default_route.has_value());
  RF_CHECK_EQ(default_route.value().render(), std::string("0.0.0.0/0"));
}

RF_TEST(ipv4_prefixes_reject_malformed_input) {
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0/33").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.256/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0.0/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.00.0/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("010.0.0.0/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0/024").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0/-1").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0/24/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.a/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix(" 10.0.0.0/24").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("10.0.0.0/24 ").has_value());
  RF_CHECK(!Destination::parse_ipv4_prefix("").has_value());
}

RF_TEST(ipv6_prefixes_parse_and_compress_canonically) {
  const Expected<Destination> compressed = Destination::parse_ipv6_prefix("2001:db8::1/64");
  RF_REQUIRE(compressed.has_value());
  RF_CHECK_EQ(compressed.value().render(), std::string("2001:db8::/64"));

  const Expected<Destination> full = Destination::parse_ipv6_prefix("2001:0db8:0000:0000:0000:0000:0000:0001/128");
  RF_REQUIRE(full.has_value());
  RF_CHECK_EQ(full.value().render(), std::string("2001:db8::1/128"));

  const Expected<Destination> all_zeros = Destination::parse_ipv6_prefix("::/0");
  RF_REQUIRE(all_zeros.has_value());
  RF_CHECK_EQ(all_zeros.value().render(), std::string("::/0"));

  const Expected<Destination> embedded = Destination::parse_ipv6_prefix("::ffff:192.168.0.1/128");
  RF_REQUIRE(embedded.has_value());
  RF_CHECK_EQ(embedded.value().render(), std::string("::ffff:c0a8:1/128"));

  // The leftmost longest run of zero groups is the one that is compressed.
  const Expected<Destination> two_runs = Destination::parse_ipv6_prefix("2001:0:0:1:0:0:0:1/128");
  RF_REQUIRE(two_runs.has_value());
  RF_CHECK_EQ(two_runs.value().render(), std::string("2001:0:0:1::1/128"));
}

RF_TEST(ipv6_prefixes_reject_malformed_input) {
  RF_CHECK(!Destination::parse_ipv6_prefix("2001:db8::1").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("2001:db8::1/129").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("2001::db8::1/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("12345::/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("2001:db8:0:0:0:0:0/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("2001:db8:0:0:0:0:0:0:0/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("2001:db8::1%eth0/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix("g2001::/64").has_value());
  RF_CHECK(!Destination::parse_ipv6_prefix(":/64").has_value());
}

RF_TEST(destination_identity_is_canonical) {
  const Destination first = Destination::parse_ipv4_prefix("10.1.2.3/16").value();
  const Destination second = Destination::parse_ipv4_prefix("10.1.0.0/16").value();
  RF_CHECK(first == second);
  RF_CHECK(first.id() == second.id());
  const Destination other = Destination::parse_ipv4_prefix("10.2.0.0/16").value();
  RF_CHECK(!(first.id() == other.id()));
  const Destination v6 = Destination::parse_ipv6_prefix("2001:db8::/64").value();
  RF_CHECK(!(first.id() == v6.id()));
}

RF_TEST(endpoint_destinations_are_bounded_tokens) {
  const Expected<Destination> service = Destination::parse_endpoint(DestinationKind::ServiceEndpoint, "payments.api", 64);
  RF_REQUIRE(service.has_value());
  RF_CHECK_EQ(service.value().render(), std::string("service-endpoint:payments.api"));
  RF_CHECK(!Destination::parse_endpoint(DestinationKind::ServiceEndpoint, "", 64).has_value());
  RF_CHECK(!Destination::parse_endpoint(DestinationKind::ServiceEndpoint, "has space", 64).has_value());
  RF_CHECK(!Destination::parse_endpoint(DestinationKind::ServiceEndpoint, std::string(65, 'a'), 64).has_value());
  RF_CHECK(!Destination::parse_endpoint(DestinationKind::Ipv4Prefix, "10.0.0.0", 64).has_value());
}

RF_TEST(destination_auto_detection_requires_explicit_kind) {
  RF_CHECK(Destination::parse("10.0.0.0/24", 128).has_value());
  RF_CHECK(Destination::parse("2001:db8::/64", 128).has_value());
  RF_CHECK(Destination::parse("fabric:leaf-1", 128).has_value());
  RF_CHECK(Destination::parse("logical:tenant-7", 128).has_value());
  RF_CHECK(!Destination::parse("10.0.0.0", 128).has_value());
  RF_CHECK(!Destination::parse("leaf-1", 128).has_value());
  RF_CHECK(!Destination::parse("", 128).has_value());
  RF_CHECK(!Destination::parse("bogus:value", 128).has_value());
}

RF_TEST(destination_encoding_rejects_non_canonical_and_impossible_values) {
  // A prefix encoding with host bits set is not canonical and must be rejected.
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(DestinationKind::Ipv4Prefix));
  writer.u8(24);
  writer.u8(10);
  writer.u8(0);
  writer.u8(0);
  writer.u8(1);
  for (int index = 0; index < 12; ++index) {
    writer.u8(0);
  }
  writer.string("");
  ByteReader reader(writer.buffer());
  Destination destination;
  RF_CHECK(!Destination::read(reader, 128, destination));

  // An impossible prefix length is rejected.
  ByteWriter length_writer;
  length_writer.u8(static_cast<std::uint8_t>(DestinationKind::Ipv4Prefix));
  length_writer.u8(33);
  for (int index = 0; index < 16; ++index) {
    length_writer.u8(0);
  }
  length_writer.string("");
  ByteReader length_reader(length_writer.buffer());
  RF_CHECK(!Destination::read(length_reader, 128, destination));

  // A malformed destination kind is rejected.
  ByteWriter kind_writer;
  kind_writer.u8(99);
  kind_writer.u8(0);
  for (int index = 0; index < 16; ++index) {
    kind_writer.u8(0);
  }
  kind_writer.string("");
  ByteReader kind_reader(kind_writer.buffer());
  RF_CHECK(!Destination::read(kind_reader, 128, destination));
}

RF_TEST(destination_encoding_round_trips) {
  const Destination original = Destination::parse_ipv6_prefix("2001:db8:1::/48").value();
  ByteWriter writer;
  original.write(writer);
  ByteReader reader(writer.buffer());
  Destination decoded;
  RF_REQUIRE(Destination::read(reader, 128, decoded));
  RF_CHECK(reader.at_end());
  RF_CHECK(decoded == original);
  RF_CHECK_EQ(decoded.render(), original.render());
}

RF_TEST(scope_coverage_is_containment_not_route_lookup) {
  const Destination aggregate = Destination::parse_ipv4_prefix("10.0.0.0/8").value();
  const Destination inside = Destination::parse_ipv4_prefix("10.1.2.0/24").value();
  const Destination outside = Destination::parse_ipv4_prefix("11.0.0.0/24").value();
  const Destination wider = Destination::parse_ipv4_prefix("10.0.0.0/4").value();
  RF_CHECK(aggregate.scope_covers(inside));
  RF_CHECK(aggregate.scope_covers(aggregate));
  RF_CHECK(!aggregate.scope_covers(outside));
  RF_CHECK(!aggregate.scope_covers(wider));
  const Destination aggregate6 = Destination::parse_ipv6_prefix("2001:db8::/32").value();
  RF_CHECK(!aggregate.scope_covers(aggregate6));
  RF_CHECK(aggregate6.scope_covers(Destination::parse_ipv6_prefix("2001:db8:1::/48").value()));
}

RF_TEST(destination_prefix_round_trip_property) {
  std::uint64_t state = 0x12345678u;
  for (int index = 0; index < 512; ++index) {
    state = mix64(state + static_cast<std::uint64_t>(index));
    const auto octet = [&state](int shift) {
      return static_cast<unsigned>((state >> shift) & 0xFFu);
    };
    const unsigned prefix = static_cast<unsigned>(state % 33u);
    const std::string text = to_decimal(octet(0)) + "." + to_decimal(octet(8)) + "." + to_decimal(octet(16)) +
                             "." + to_decimal(octet(24)) + "/" + to_decimal(prefix);
    const Expected<Destination> parsed = Destination::parse_ipv4_prefix(text);
    RF_REQUIRE(parsed.has_value());
    const Expected<Destination> again = Destination::parse_ipv4_prefix(parsed.value().render());
    RF_REQUIRE(again.has_value());
    RF_CHECK(again.value() == parsed.value());
    RF_CHECK(again.value().id() == parsed.value().id());
  }
}
