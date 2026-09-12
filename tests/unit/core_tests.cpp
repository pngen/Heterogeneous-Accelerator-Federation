// Core primitive contracts: identities, generations, versions, hashing, and
// the bounded byte codec.

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/core/bytes.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/idgen.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/core/version.hpp"
#include "haf/model/taxonomy.hpp"

using namespace haf;

HAF_TEST(core, identity_round_trip) {
    const Id128 value = derive_identity("haf.test", "alpha");
    const std::string hex = value.to_hex();
    HAF_EQ(hex.size(), std::size_t{32});
    const std::optional<Id128> parsed = Id128::parse(hex);
    HAF_CHECK(parsed.has_value());
    HAF_CHECK(*parsed == value);
    HAF_CHECK(!value.is_nil());
    HAF_CHECK(Id128::nil().is_nil());
}

HAF_TEST(core, identity_parse_rejects_malformed) {
    HAF_CHECK(!Id128::parse("").has_value());
    HAF_CHECK(!Id128::parse("zzzz").has_value());
    HAF_CHECK(!Id128::parse("00112233445566778899aabbccddee").has_value());   // 30 digits
    HAF_CHECK(!Id128::parse("00112233445566778899aabbccddeeff0").has_value()); // 33 digits
    HAF_CHECK(!Id128::parse("00112233445566778899aabbccddeefg").has_value());  // non-hex
}

HAF_TEST(core, identity_derivation_is_pure) {
    // The same domain and token must produce the same identity in any process.
    HAF_CHECK(derive_identity("haf.vendor", "nvidia") == derive_identity("haf.vendor", "nvidia"));
    HAF_CHECK(derive_identity("haf.vendor", "nvidia") != derive_identity("haf.vendor", "amd"));
    HAF_CHECK(derive_identity("haf.vendor", "nvidia") != derive_identity("haf.architecture", "nvidia"));
}

HAF_TEST(core, strong_ids_are_distinct_types) {
    const AcceleratorId accelerator = AcceleratorId::from_raw(derive_identity("haf.accelerator", "a"));
    const AgentId agent = AgentId::from_raw(derive_identity("haf.accelerator", "a"));
    // Same underlying 128-bit value, independent identities: the types exist so
    // that the compiler refuses to interchange them.
    HAF_CHECK(accelerator.raw() == agent.raw());
    HAF_CHECK(accelerator.to_string() == agent.to_string());
    HAF_CHECK(accelerator == AcceleratorId::from_raw(derive_identity("haf.accelerator", "a")));
}

HAF_TEST(core, generation_never_wraps_to_zero) {
    Generation<FederationGenerationTag> generation;
    HAF_CHECK(generation.is_initial());
    generation = generation.next();
    HAF_EQ(generation.value(), std::uint64_t{1});
    const Generation<FederationGenerationTag> maximum(Generation<FederationGenerationTag>::kMaxValue);
    HAF_CHECK(maximum.next().value() == Generation<FederationGenerationTag>::kMaxValue);
}

HAF_TEST(core, id_generator_never_repeats) {
    IdGenerator generator = make_deterministic_id_generator(12345);
    std::vector<std::string> seen;
    seen.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        seen.push_back(generator.next().to_hex());
    }
    std::sort(seen.begin(), seen.end());
    HAF_CHECK(std::unique(seen.begin(), seen.end()) == seen.end());
}

HAF_TEST(core, id_generator_is_deterministic_for_a_seed) {
    IdGenerator left = make_deterministic_id_generator(7);
    IdGenerator right = make_deterministic_id_generator(7);
    for (int i = 0; i < 32; ++i) {
        HAF_CHECK(left.next() == right.next());
    }
}

HAF_TEST(core, semantic_version_ordering_follows_semver) {
    const auto parse = [](const char* text) { return SemanticVersion::parse(text).value(); };
    HAF_CHECK(parse("1.0.0") < parse("2.0.0"));
    HAF_CHECK(parse("1.0.0") < parse("1.1.0"));
    HAF_CHECK(parse("1.0.0") < parse("1.0.1"));
    HAF_CHECK(parse("1.0.0-alpha") < parse("1.0.0"));
    HAF_CHECK(parse("1.0.0-alpha") < parse("1.0.0-beta"));
    HAF_CHECK(parse("1.0.0-alpha.1") < parse("1.0.0-alpha.2"));
    HAF_CHECK(parse("1.0.0-2") < parse("1.0.0-10"));
    // Build metadata carries no precedence.
    HAF_CHECK(parse("1.0.0+build.1") == parse("1.0.0+build.2"));
    HAF_CHECK(parse("1.0.0+build.1").to_string() == std::string("1.0.0"));
}

HAF_TEST(core, semantic_version_rejects_malformed) {
    HAF_CHECK(!SemanticVersion::parse("").has_value());
    HAF_CHECK(!SemanticVersion::parse("1").has_value());
    HAF_CHECK(!SemanticVersion::parse("1.2").has_value());
    HAF_CHECK(!SemanticVersion::parse("1.2.3.4").has_value());
    HAF_CHECK(!SemanticVersion::parse("a.b.c").has_value());
    HAF_CHECK(!SemanticVersion::parse("1.2.3-").has_value());
    HAF_CHECK(!SemanticVersion::parse("1.2.3+").has_value());
    HAF_CHECK(!SemanticVersion::parse("1.2.-3").has_value());
}

HAF_TEST(core, version_range_satisfaction) {
    const auto range = [](const char* text) { return VersionRange::parse(text).value(); };
    const auto version = [](const char* text) { return SemanticVersion::parse(text).value(); };
    HAF_CHECK(range(">=12.0.0 <13.0.0").matches(version("12.9.0")));
    HAF_CHECK(!range(">=12.0.0 <13.0.0").matches(version("13.0.0")));
    HAF_CHECK(range("^1.2.3").matches(version("1.9.0")));
    HAF_CHECK(!range("^1.2.3").matches(version("2.0.0")));
    HAF_CHECK(range("~1.2.3").matches(version("1.2.9")));
    HAF_CHECK(!range("~1.2.3").matches(version("1.3.0")));
    HAF_CHECK(range("*").matches(version("0.0.1")));
    HAF_CHECK(range("").matches(version("0.0.1")));
    HAF_CHECK(range("!=1.2.3").matches(version("1.2.4")));
    HAF_CHECK(!range("!=1.2.3").matches(version("1.2.3")));
}

HAF_TEST(core, version_range_rejects_malformed) {
    HAF_CHECK(!VersionRange::parse(">=").ok());
    HAF_CHECK(!VersionRange::parse(">=x.y.z").ok());
    HAF_CHECK(!VersionRange::parse("===").ok());
    HAF_CHECK(VersionRange::parse(">=1.0.0").ok());
    // A bare version is a legitimate equality clause, not a malformed range.
    const Result<VersionRange> bare = VersionRange::parse("12.0.0");
    HAF_REQUIRE_OK(bare);
    HAF_CHECK(bare->matches(SemanticVersion::parse("12.0.0").value()));
    HAF_CHECK(!bare->matches(SemanticVersion::parse("12.0.1").value()));
}

HAF_TEST(core, fnv1a_matches_known_vectors) {
    Fnv1a64 hasher;
    hasher.update(std::string_view(""));
    HAF_EQ(hasher.digest(), Fnv1a64::kOffsetBasis);
    Fnv1a64 second;
    second.update(std::string_view("a"));
    HAF_EQ(second.digest(), std::uint64_t{0xaf63dc4c8601ec8cULL});
}

HAF_TEST(core, sha256_matches_known_vectors) {
    HAF_EQ(to_hex(Sha256::hash(std::string_view(""))),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    HAF_EQ(to_hex(Sha256::hash(std::string_view("abc"))),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // A long input exercises the multi-block path and the length encoding.
    std::string long_input(1000, 'a');
    HAF_EQ(to_hex(Sha256::hash(long_input)),
           std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

HAF_TEST(core, byte_codec_round_trip) {
    ByteWriter writer;
    writer.u8(0xABU);
    writer.boolean(true);
    writer.u16(0x1234U);
    writer.u32(0xDEADBEEFU);
    writer.u64(0x0123456789ABCDEFULL);
    writer.i64(-42);
    writer.f64(3.5);
    writer.string("hello world");
    writer.id128(derive_identity("haf.test", "value"));

    ByteReader reader(writer.data());
    std::uint8_t u8 = 0;
    bool boolean_value = false;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::int64_t i64 = 0;
    double f64 = 0.0;
    std::string text;
    Id128 id;
    HAF_CHECK(reader.u8(u8));
    HAF_CHECK(reader.boolean(boolean_value));
    HAF_CHECK(reader.u16(u16));
    HAF_CHECK(reader.u32(u32));
    HAF_CHECK(reader.u64(u64));
    HAF_CHECK(reader.i64(i64));
    HAF_CHECK(reader.f64(f64));
    HAF_CHECK(reader.string(text, 64));
    HAF_CHECK(reader.id128(id));
    HAF_EQ(u8, std::uint8_t{0xAB});
    HAF_CHECK(boolean_value);
    HAF_EQ(u16, std::uint16_t{0x1234});
    HAF_EQ(u32, std::uint32_t{0xDEADBEEF});
    HAF_EQ(u64, std::uint64_t{0x0123456789ABCDEFULL});
    HAF_EQ(i64, std::int64_t{-42});
    HAF_CHECK(f64 == 3.5);
    HAF_EQ(text, std::string("hello world"));
    HAF_CHECK(id == derive_identity("haf.test", "value"));
    HAF_EQ(reader.remaining(), std::size_t{0});
}

HAF_TEST(core, byte_reader_rejects_truncation_and_oversize) {
    ByteWriter writer;
    writer.u32(9999);
    ByteReader reader(writer.data());
    std::uint64_t value = 0;
    HAF_CHECK(!reader.u64(value));
    HAF_CHECK(reader.failed());
    HAF_EQ(static_cast<int>(reader.error().code()), static_cast<int>(ErrorCode::TruncatedFrame));

    ByteWriter declared;
    declared.u32(1U << 20);
    ByteReader second(declared.data());
    std::string text;
    HAF_CHECK(!second.string(text, 64));
    HAF_EQ(static_cast<int>(second.error().code()), static_cast<int>(ErrorCode::BoundsExceeded));
}

HAF_TEST(core, byte_reader_rejects_non_finite_scalars) {
    ByteWriter writer;
    writer.f64(std::numeric_limits<double>::quiet_NaN());
    ByteReader reader(writer.data());
    double value = 0.0;
    HAF_CHECK(!reader.f64(value));
    HAF_EQ(static_cast<int>(reader.error().code()), static_cast<int>(ErrorCode::NonFiniteQuantity));
}

HAF_TEST(core, byte_reader_enforces_boolean_domain) {
    ByteWriter writer;
    writer.u8(7);
    ByteReader reader(writer.data());
    bool value = false;
    HAF_CHECK(!reader.boolean(value));
    HAF_CHECK(reader.failed());
}

HAF_TEST(core, count_guard_rejects_impossible_counts) {
    ByteWriter writer;
    writer.u32(1000000);
    ByteReader reader(writer.data());
    std::uint32_t count = 0;
    HAF_CHECK(!reader.count(count, 4096, 8));
    HAF_EQ(static_cast<int>(reader.error().code()), static_cast<int>(ErrorCode::BoundsExceeded));

    ByteWriter second_writer;
    second_writer.u32(100);
    ByteReader second(second_writer.data());
    HAF_CHECK(!second.count(count, 4096, 8));
    HAF_EQ(static_cast<int>(second.error().code()), static_cast<int>(ErrorCode::TruncatedFrame));
}

HAF_TEST(core, utf8_validation) {
    HAF_CHECK(is_valid_utf8(""));
    HAF_CHECK(is_valid_utf8("plain ascii"));
    HAF_CHECK(is_valid_utf8("caf\xC3\xA9"));
    HAF_CHECK(!is_valid_utf8("\xC3"));
    HAF_CHECK(!is_valid_utf8("\xFF\xFE"));
    HAF_CHECK(!is_valid_utf8("\xED\xA0\x80"));  // surrogate half
}

HAF_TEST(core, text_escaping_is_deterministic) {
    HAF_EQ(escape_text("plain"), std::string("plain"));
    HAF_EQ(escape_text("a\"b"), std::string("a\\\"b"));
    HAF_EQ(escape_text("line\nbreak"), std::string("line\\nbreak"));
    HAF_EQ(escape_text("\x01"), std::string("\\x01"));
}

HAF_TEST(core, error_codes_have_stable_names) {
    HAF_EQ(std::string(to_string(ErrorCode::Ok)), std::string("Ok"));
    HAF_EQ(std::string(to_string(ErrorCode::StaleEpoch)), std::string("StaleEpoch"));
    HAF_CHECK(is_staleness(ErrorCode::StaleBoot));
    HAF_CHECK(is_staleness(ErrorCode::StaleCapabilityGeneration));
    HAF_CHECK(!is_staleness(ErrorCode::VendorMismatch));
    HAF_CHECK(is_compatibility_outcome(ErrorCode::InsufficientMemory));
    HAF_CHECK(!is_compatibility_outcome(ErrorCode::ProtocolViolation));
}

HAF_TEST(core, status_and_result_semantics) {
    const Status status = Status::failure(ErrorCode::StaleEpoch, "epoch moved");
    HAF_CHECK(!status.ok());
    HAF_EQ(static_cast<int>(status.code()), static_cast<int>(ErrorCode::StaleEpoch));
    HAF_CHECK(status.describe().find("StaleEpoch") == 0);

    const Result<int> good(42);
    HAF_CHECK(good.ok());
    HAF_EQ(*good, 42);
    const Result<int> bad = Status::failure(ErrorCode::NotFound, "missing");
    HAF_CHECK(!bad.ok());
    HAF_EQ(bad.value_or(7), 7);
}

HAF_TEST(core, timestamp_formatting_is_deterministic) {
    HAF_EQ(format_timestamp(from_unix_nanos(0)), std::string("1970-01-01T00:00:00.000Z"));
    HAF_EQ(format_timestamp(from_unix_nanos(1'700'000'000'500'000'000LL)),
           std::string("2023-11-14T22:13:20.500Z"));
    HAF_EQ(format_duration_nanos(500), std::string("500ns"));
}
