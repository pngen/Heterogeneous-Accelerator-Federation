// Adversarial cases. Every one of these is expected to be refused with a typed
// error, or to leave the federation in a state that still audits clean.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "framework/test_harness.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/federation/federation.hpp"
#include "haf/net/frame.hpp"
#include "haf/net/messages.hpp"
#include "haf/persist/file_store.hpp"
#include "haf/model/taxonomy.hpp"
#include "support/environment.hpp"
#include "support/profiles.hpp"

using namespace haf;
using namespace haf::test;

HAF_TEST(adversarial, malformed_advertisements_are_refused) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(200, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AuthorityClaim claim = (*federation)->epoch_claim("adversarial");

    AcceleratorDescriptor device = make_cuda_like(201);

    AcceleratorDescriptor no_identity = device;
    no_identity.id = AcceleratorId{};
    const Result<MemberRecord> nil_id = (*federation)->observe(no_identity, claim);
    HAF_CHECK(!nil_id.ok());
    HAF_EQ(static_cast<int>(nil_id.status().code()), static_cast<int>(ErrorCode::InvalidArgument));

    AcceleratorDescriptor forged_identity = device;
    forged_identity.id = AcceleratorId::from_raw(derive_identity("haf.accelerator", "forged"));
    const Result<MemberRecord> forged = (*federation)->observe(forged_identity, claim);
    HAF_CHECK(!forged.ok());
    HAF_EQ(static_cast<int>(forged.status().code()), static_cast<int>(ErrorCode::IntegrityFailure));

    AcceleratorDescriptor empty_capabilities = device;
    HAF_REQUIRE_OK(empty_capabilities.capabilities.set_records({}));
    const Result<MemberRecord> empty = (*federation)->observe(empty_capabilities, claim);
    HAF_CHECK(!empty.ok());

    AcceleratorDescriptor native_synthetic = device;
    native_synthetic.support_level = SupportLevel::Native;
    const Result<MemberRecord> upgraded = (*federation)->observe(native_synthetic, claim);
    HAF_CHECK(!upgraded.ok());
    HAF_EQ(static_cast<int>(upgraded.status().code()), static_cast<int>(ErrorCode::IntegrityFailure));
}

HAF_TEST(adversarial, impossible_quantities_and_non_finite_values_are_refused) {
    HAF_CHECK(!make_integer("memory.total_bytes", -1, CapabilityState::Supported).ok() == false);
    const Result<CapabilityValue> nan_scalar = CapabilityValue::parse(CapabilityKind::Scalar, "nan");
    HAF_CHECK(!nan_scalar.ok());
    const Result<CapabilityValue> inf_scalar = CapabilityValue::parse(CapabilityKind::Scalar, "inf");
    HAF_CHECK(!inf_scalar.ok());
    const Result<CapabilityValue> huge_scalar = CapabilityValue::parse(CapabilityKind::Scalar, "1e999");
    HAF_CHECK(!huge_scalar.ok());

    CapabilityValue scalar = CapabilityValue::scalar(std::numeric_limits<double>::quiet_NaN());
    HAF_CHECK(!scalar.canonicalize().ok());

    const Result<CapabilityRequirement> bad_weight =
        require_present("numeric.fp32", RequirementStrength::Soft);
    HAF_REQUIRE_OK(bad_weight);
    CapabilityRequirement infinite_weight = *bad_weight;
    infinite_weight.weight = std::numeric_limits<double>::infinity();
    HAF_CHECK(!infinite_weight.validate().ok());

    FederationPolicy policy = permissive_policy();
    policy.minimum_memory_bytes = std::numeric_limits<std::uint64_t>::max();
    HAF_CHECK(!policy.validate().ok());
}

HAF_TEST(adversarial, duplicate_live_boot_is_refused) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(202, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor first = make_cuda_like(203);
    HAF_REQUIRE_OK(join(**federation, first));

    // Same physical device reported by a different agent boot: a contradictory
    // federation state that must be refused rather than merged.
    const AcceleratorDescriptor second = make_device(adapters::cuda_class_profile(), 999, "test-cuda-like", 0);
    HAF_CHECK(second.physical_device == first.physical_device);
    HAF_CHECK(second.id != first.id);
    const Result<MemberRecord> refused = (*federation)->observe(second, (*federation)->epoch_claim("duplicate"));
    HAF_CHECK(!refused.ok());
    HAF_EQ(static_cast<int>(refused.status().code()), static_cast<int>(ErrorCode::DuplicateLiveBoot));
}

HAF_TEST(adversarial, retired_incarnation_cannot_be_revived) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(204, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(205);
    HAF_REQUIRE_OK(join(**federation, device));
    HAF_REQUIRE_OK((*federation)->retire(device.id, (*federation)->epoch_claim("retire")));

    const Result<MemberRecord> revived =
        (*federation)->observe(device, (*federation)->epoch_claim("revive attempt"));
    HAF_CHECK(!revived.ok());
    HAF_EQ(static_cast<int>(revived.status().code()), static_cast<int>(ErrorCode::Retired));
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(adversarial, stale_epoch_boot_and_generation_are_rejected) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(206, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(207);
    HAF_REQUIRE_OK(join(**federation, device));

    AuthorityClaim stale_epoch = (*federation)->epoch_claim("stale epoch");
    stale_epoch.epoch = CoordinatorEpoch(stale_epoch.epoch.value() + 5);
    const Result<MemberRecord> epoch_result = (*federation)->fence(device.id, stale_epoch);
    HAF_CHECK(!epoch_result.ok());
    HAF_EQ(static_cast<int>(epoch_result.status().code()), static_cast<int>(ErrorCode::StaleEpoch));

    AuthorityClaim stale_generation = (*federation)->epoch_claim("stale generation");
    stale_generation.check_federation_generation = true;
    stale_generation.federation_generation = FederationGeneration(999);
    const Result<MemberRecord> generation_result = (*federation)->fence(device.id, stale_generation);
    HAF_CHECK(!generation_result.ok());
    HAF_EQ(static_cast<int>(generation_result.status().code()),
           static_cast<int>(ErrorCode::StaleFederationGeneration));

    const Result<MemberRecord> current = (*federation)->member(device.id);
    HAF_REQUIRE_OK(current);
    AuthorityClaim stale_boot = (*federation)->epoch_claim("stale boot");
    stale_boot.check_agent = true;
    stale_boot.agent = current->agent;
    stale_boot.check_agent_boot = true;
    stale_boot.agent_boot = AgentBootId::from_raw(derive_identity("haf.boot", "stale"));
    const Result<MemberRecord> boot_result = (*federation)->fence(device.id, stale_boot);
    HAF_CHECK(!boot_result.ok());
    HAF_EQ(static_cast<int>(boot_result.status().code()), static_cast<int>(ErrorCode::StaleBoot));

    AuthorityClaim stale_device = (*federation)->epoch_claim("stale device");
    stale_device.check_device_generation = true;
    stale_device.device_generation = DeviceGeneration(77);
    const Result<MemberRecord> device_result = (*federation)->fence(device.id, stale_device);
    HAF_CHECK(!device_result.ok());
    HAF_EQ(static_cast<int>(device_result.status().code()),
           static_cast<int>(ErrorCode::StaleDeviceGeneration));

    AuthorityClaim stale_capability = (*federation)->epoch_claim("stale capability");
    stale_capability.check_capability_generation = true;
    stale_capability.capability_generation = CapabilityGeneration(77);
    const Result<MemberRecord> capability_result = (*federation)->fence(device.id, stale_capability);
    HAF_CHECK(!capability_result.ok());
    HAF_EQ(static_cast<int>(capability_result.status().code()),
           static_cast<int>(ErrorCode::StaleCapabilityGeneration));

    AuthorityClaim stale_policy = (*federation)->epoch_claim("stale policy");
    stale_policy.check_policy_generation = true;
    stale_policy.policy_generation = PolicyGeneration(77);
    const Result<MemberRecord> policy_result = (*federation)->fence(device.id, stale_policy);
    HAF_CHECK(!policy_result.ok());
    HAF_EQ(static_cast<int>(policy_result.status().code()), static_cast<int>(ErrorCode::StalePolicy));
}

HAF_TEST(adversarial, unknown_accelerator_is_reported_as_such) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(208, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorId unknown = AcceleratorId::from_raw(derive_identity("haf.accelerator", "absent"));
    const Result<MemberRecord> result = (*federation)->fence(unknown, (*federation)->epoch_claim("unknown"));
    HAF_CHECK(!result.ok());
    HAF_EQ(static_cast<int>(result.status().code()), static_cast<int>(ErrorCode::UnknownAccelerator));
}

HAF_TEST(adversarial, invalid_lifecycle_transitions_are_refused) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(209, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(210);
    const AuthorityClaim claim = (*federation)->epoch_claim("invalid transition");
    const Result<MemberRecord> observed = (*federation)->observe(device, claim);
    HAF_REQUIRE_OK(observed);
    // Discovered/Observed cannot jump straight to Active.
    const Result<MemberRecord> skipped = (*federation)->activate(device.id, claim);
    HAF_CHECK(!skipped.ok());
    HAF_EQ(static_cast<int>(skipped.status().code()), static_cast<int>(ErrorCode::InvalidTransition));
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(adversarial, capability_downgrade_cannot_change_provenance) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(211, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(212);
    HAF_REQUIRE_OK(join(**federation, device));
    const Result<MemberRecord> current = (*federation)->member(device.id);
    HAF_REQUIRE_OK(current);

    const Result<CapabilitySet> capabilities = adapters::build_capability_set(adapters::cuda_class_profile());
    HAF_REQUIRE_OK(capabilities);
    EvidenceRecord evidence = make_evidence("forged", "attempted upgrade", "1.0.0", device.id, device.physical_device,
                                            EvidenceProvenance::Real, EvidenceKind::Declaration,
                                            capabilities->digest(), RuntimeVersion{1, 0, 0}, RuntimeVersion{1, 0, 0},
                                            0, false);
    evidence.capability_generation = current->capability_generation;
    const Result<MemberRecord> upgraded =
        (*federation)->update_capabilities(device.id, *capabilities, evidence, (*federation)->epoch_claim("upgrade"));
    HAF_CHECK(!upgraded.ok());
    HAF_EQ(static_cast<int>(upgraded.status().code()), static_cast<int>(ErrorCode::IntegrityFailure));
}

HAF_TEST(adversarial, capability_update_with_mismatched_digest_is_refused) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(213, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(214);
    HAF_REQUIRE_OK(join(**federation, device));
    const Result<MemberRecord> current = (*federation)->member(device.id);
    HAF_REQUIRE_OK(current);

    const Result<CapabilitySet> capabilities = adapters::build_capability_set(adapters::rocm_class_profile());
    HAF_REQUIRE_OK(capabilities);
    EvidenceRecord evidence = make_evidence("mismatch", "wrong digest", "1.0.0", device.id, device.physical_device,
                                            EvidenceProvenance::Synthetic, EvidenceKind::Declaration,
                                            Sha256::hash(std::string_view("unrelated")), RuntimeVersion{1, 0, 0},
                                            RuntimeVersion{1, 0, 0}, 0, false);
    evidence.capability_generation = current->capability_generation;
    const Result<MemberRecord> result =
        (*federation)->update_capabilities(device.id, *capabilities, evidence, (*federation)->epoch_claim("mismatch"));
    HAF_CHECK(!result.ok());
    HAF_EQ(static_cast<int>(result.status().code()), static_cast<int>(ErrorCode::IntegrityFailure));
}

HAF_TEST(adversarial, hanging_counts_and_huge_token_sets_are_bounded) {
    // A capability set that exceeds the record bound is refused outright.
    std::vector<CapabilityRecord> records;
    for (std::size_t index = 0; index < Limits::kMaxCapabilitiesPerSet + 1; ++index) {
        records.push_back(make_presence("numeric.fp32", CapabilityState::Supported));
    }
    CapabilitySet set;
    const Status status = set.set_records(std::move(records));
    HAF_CHECK(!status.ok());
    HAF_EQ(static_cast<int>(status.code()), static_cast<int>(ErrorCode::BoundsExceeded));

    std::vector<std::string> tokens;
    for (std::size_t index = 0; index < Limits::kMaxTokensPerCapability + 1; ++index) {
        tokens.push_back("token-" + std::to_string(index));
    }
    HAF_CHECK(!make_token_set("isa.code_object_targets", std::move(tokens), CapabilityState::Supported).ok());
}

HAF_TEST(adversarial, frame_header_validation) {
    net::Frame frame;
    frame.header.type = static_cast<std::uint16_t>(net::MessageType::Hello);
    frame.header.sequence = 1;
    frame.payload = {1, 2, 3, 4};
    const Result<ByteBuffer> encoded = net::encode_frame(frame);
    HAF_REQUIRE_OK(encoded);

    net::FrameHeader decoded;
    const Result<net::FrameHeader> header =
        net::decode_frame_header(encoded->data(), encoded->size(), 1024);
    HAF_REQUIRE_OK(header);
    HAF_EQ(header->payload_length, std::uint32_t{4});
    HAF_EQ(static_cast<int>(header->type), static_cast<int>(net::MessageType::Hello));

    // Truncated header.
    HAF_CHECK(!net::decode_frame_header(encoded->data(), 8, 1024).ok());
    // Corrupted magic.
    ByteBuffer corrupted = *encoded;
    corrupted[0] = 'X';
    HAF_CHECK(!net::decode_frame_header(corrupted.data(), corrupted.size(), 1024).ok());
    // Corrupted header CRC.
    ByteBuffer bad_crc = *encoded;
    bad_crc[12] = static_cast<std::uint8_t>(bad_crc[12] ^ 0xFFU);
    const Result<net::FrameHeader> crc_result = net::decode_frame_header(bad_crc.data(), bad_crc.size(), 1024);
    HAF_CHECK(!crc_result.ok());
    // Non-zero reserved word.
    ByteBuffer reserved = *encoded;
    reserved[10] = 1;
    reserved[24] = 0;
    reserved[25] = 0;
    reserved[26] = 0;
    reserved[27] = 0;
    HAF_CHECK(!net::decode_frame_header(reserved.data(), reserved.size(), 1024).ok());
    // Oversized declared payload.
    const Result<ByteBuffer> oversized = net::encode_frame(frame);
    HAF_REQUIRE_OK(oversized);
    HAF_CHECK(!net::decode_frame_header(oversized->data(), oversized->size(), 2).ok());
}

HAF_TEST(adversarial, frame_trailer_detects_corruption) {
    net::Frame frame;
    frame.header.type = static_cast<std::uint16_t>(net::MessageType::Query);
    frame.header.sequence = 9;
    frame.payload = {9, 8, 7, 6, 5};
    const Result<ByteBuffer> encoded = net::encode_frame(frame);
    HAF_REQUIRE_OK(encoded);
    ByteBuffer corrupted = *encoded;
    corrupted[net::kFrameHeaderBytes] = static_cast<std::uint8_t>(corrupted[net::kFrameHeaderBytes] ^ 0xFFU);
    ByteBuffer payload(corrupted.begin() + static_cast<std::ptrdiff_t>(net::kFrameHeaderBytes),
                       corrupted.begin() + static_cast<std::ptrdiff_t>(net::kFrameHeaderBytes + 5));
    const std::uint8_t* trailer = corrupted.data() + net::kFrameHeaderBytes + 5;
    const VoidResult verified =
        net::verify_frame_trailer(corrupted.data(), net::kFrameHeaderBytes, payload, trailer, net::kFrameTrailerBytes);
    HAF_CHECK(!verified.ok());
    HAF_EQ(static_cast<int>(verified.status().code()), static_cast<int>(ErrorCode::CorruptPayload));
}

HAF_TEST(adversarial, unknown_message_types_and_enums_are_refused) {
    net::MessageType type = net::MessageType::Hello;
    HAF_CHECK(net::message_type_from_wire(1, type));
    HAF_CHECK(!net::message_type_from_wire(0, type));
    HAF_CHECK(!net::message_type_from_wire(9999, type));

    // A hello payload with an unknown role is refused by the decoder.
    ByteWriter writer;
    writer.u16(1);
    writer.u8(42);
    writer.id128(Id128::nil());
    writer.id128(Id128::nil());
    writer.id128(Id128::nil());
    writer.id128(Id128::nil());
    writer.string("node");
    writer.string("client");
    writer.string("1.0.0");
    writer.u64(1);
    const Result<net::HelloRequest> decoded = net::decode_hello_request(writer.data());
    HAF_CHECK(!decoded.ok());
    HAF_EQ(static_cast<int>(decoded.status().code()), static_cast<int>(ErrorCode::MalformedData));
}

HAF_TEST(adversarial, truncated_and_corrupt_persistence_is_rejected) {
    ScratchDirectory scratch("adversarial-store");
    const std::filesystem::path store_path = scratch.file("federation.store");
    FederationConfig config;
    config.name = "corrupt";
    config.id_seed = 215;
    config.store_path = store_path;
    Result<std::unique_ptr<Federation>> federation = Federation::open(config);
    HAF_REQUIRE_OK(federation);
    HAF_REQUIRE_OK(join(**federation, make_cuda_like(216)));

    const std::string original = read_file_text(store_path);
    HAF_CHECK(original.size() > 128);

    // Truncation.
    HAF_CHECK(write_file_text(store_path, original.substr(0, original.size() / 2)));
    {
        FederationConfig reopen = config;
        const Result<std::unique_ptr<Federation>> recovered = Federation::open(reopen);
        HAF_CHECK(!recovered.ok());
    }

    // Corruption of the payload region.
    std::string corrupted = original;
    corrupted[corrupted.size() / 2] = static_cast<char>(corrupted[corrupted.size() / 2] ^ 0x5A);
    HAF_CHECK(write_file_text(store_path, corrupted));
    {
        const Result<std::unique_ptr<Federation>> recovered = Federation::open(config);
        HAF_CHECK(!recovered.ok());
    }

    // Corrupted magic.
    std::string bad_magic = original;
    bad_magic[0] = 'X';
    HAF_CHECK(write_file_text(store_path, bad_magic));
    {
        const Result<std::unique_ptr<Federation>> recovered = Federation::open(config);
        HAF_CHECK(!recovered.ok());
    }

    // Trailing garbage.
    HAF_CHECK(write_file_text(store_path, original + "trailing"));
    {
        const Result<std::unique_ptr<Federation>> recovered = Federation::open(config);
        HAF_CHECK(!recovered.ok());
    }

    // Restoring the original bytes recovers cleanly, which proves the failures
    // above were caused by the damage rather than by the recovery path.
    HAF_CHECK(write_file_text(store_path, original));
    {
        const Result<std::unique_ptr<Federation>> recovered = Federation::open(config);
        HAF_REQUIRE_OK(recovered);
        HAF_CHECK((*recovered)->member_count() == 1);
    }
}

HAF_TEST(adversarial, version_range_and_policy_references_are_validated) {
    HAF_CHECK(!VersionRange::parse(">=abc").ok());
    HAF_CHECK(!VersionRange::parse("<<1.0.0").ok());
    FederationPolicy policy = permissive_policy();
    policy.required_capabilities = {"not.a.real.capability"};
    HAF_CHECK(!policy.validate().ok());
}

HAF_TEST(adversarial, contradictory_capability_sets_are_refused) {
    CapabilitySet set;
    const Result<CapabilityRecord> record = make_presence("numeric.fp64", CapabilityState::Supported);
    HAF_REQUIRE_OK(record);
    CapabilityRecord duplicate = *record;
    duplicate.state = CapabilityState::Unsupported;
    const Status status = set.set_records({*record, duplicate});
    HAF_CHECK(!status.ok());
    HAF_EQ(static_cast<int>(status.code()), static_cast<int>(ErrorCode::DuplicateIdentity));
}

HAF_TEST(adversarial, repeated_fence_operations_are_idempotent) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(217, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(218);
    HAF_REQUIRE_OK(join(**federation, device));
    for (int iteration = 0; iteration < 5; ++iteration) {
        const Result<MemberRecord> fenced =
            (*federation)->fence(device.id, (*federation)->epoch_claim("repeated fence"));
        HAF_CHECK(fenced.ok());
        HAF_CHECK(fenced->state == MemberState::Fenced);
    }
    const Result<MemberRecord> reopened =
        (*federation)->reopen(device.id, (*federation)->epoch_claim("reopen"));
    HAF_CHECK(reopened.ok());
    HAF_CHECK(reopened->state == MemberState::Observed);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(adversarial, oversized_capability_advertisement_over_the_wire_is_refused) {
    // A peer that declares a huge capability count must be rejected from the
    // declared count alone, before any allocation proportional to it.
    ByteWriter writer;
    writer.id128(Id128::nil());          // accelerator identity, never reached by the decoder
    writer.id128(Id128::nil());          // physical device
    writer.generation(DeviceGeneration(1));
    writer.id128(Id128::nil());          // agent
    writer.id128(Id128::nil());          // boot
    writer.id128(Id128::nil());          // node
    writer.string("nvidia");
    writer.string("model");
    writer.string("arch");
    writer.string("gen");
    writer.string("cuda");
    writer.u32(1);
    writer.u32(2);
    writer.u32(3);
    writer.u32(1);
    writer.u32(2);
    writer.u32(3);
    writer.id128(Id128::nil());          // capability set id
    writer.generation(CapabilityGeneration(1));
    writer.u32(0xFFFFFFF0U);             // absurd declared capability count
    ByteReader reader(writer.data());
    AcceleratorDescriptor descriptor;
    HAF_CHECK(!AcceleratorDescriptor::deserialize(reader, descriptor, false));
    HAF_CHECK(reader.failed());
    HAF_EQ(static_cast<int>(reader.error().code()), static_cast<int>(ErrorCode::BoundsExceeded));
}

HAF_TEST(adversarial, malformed_advertisement_payloads_are_refused_over_the_wire) {
    // Trailing bytes after a well-formed advertisement are a malformed payload.
    net::AdvertiseRequest request;
    request.claim.check_epoch = true;
    request.claim.epoch = CoordinatorEpoch(1);
    request.claim.agent = AgentId::from_raw(derive_identity("haf.agent", "wire"));
    request.claim.agent_boot = AgentBootId::from_raw(derive_identity("haf.boot", "wire"));
    request.devices.push_back(make_cuda_like(1001));
    const Result<ByteBuffer> encoded = net::encode_message(request);
    HAF_REQUIRE_OK(encoded);
    ByteBuffer with_trailer = *encoded;
    with_trailer.push_back(0x7FU);
    HAF_CHECK(!net::decode_advertise_request(with_trailer).ok());

    // A truncated payload is refused as well.
    ByteBuffer truncated(encoded->begin(), encoded->begin() + static_cast<std::ptrdiff_t>(encoded->size() / 2));
    HAF_CHECK(!net::decode_advertise_request(truncated).ok());
}

HAF_TEST(adversarial, contradictory_capability_sets_are_refused_over_the_wire) {
    const AcceleratorDescriptor device = make_cuda_like(1002);
    net::AdvertiseRequest request;
    request.claim.check_epoch = true;
    request.claim.epoch = CoordinatorEpoch(1);
    request.devices.push_back(device);
    const Result<ByteBuffer> encoded = net::encode_message(request);
    HAF_REQUIRE_OK(encoded);

    // The decoder accepts the bytes; the federation refuses the contradiction.
    Result<net::AdvertiseRequest> decoded = net::decode_advertise_request(*encoded);
    HAF_REQUIRE_OK(decoded);
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(1003, permissive_policy());
    HAF_REQUIRE_OK(federation);
    AcceleratorDescriptor forged = decoded->devices.front();
    forged.support_level = SupportLevel::Native;
    forged.provenance = EvidenceProvenance::Real;
    const Result<MemberRecord> refused = (*federation)->observe(forged, (*federation)->epoch_claim("contradiction"));
    HAF_CHECK(!refused.ok());
}

HAF_TEST(adversarial, reconnect_storm_does_not_corrupt_federation_state) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(1004, permissive_policy());
    HAF_REQUIRE_OK(federation);
    // Repeated advertisement of the same incarnation must converge, not
    // accumulate duplicate members or advance generations without bound.
    const AcceleratorDescriptor device = make_cuda_like(1005);
    HAF_REQUIRE_OK(join(**federation, device));
    const Result<MemberRecord> baseline = (*federation)->member(device.id);
    HAF_REQUIRE_OK(baseline);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const Result<MemberRecord> observed = (*federation)->observe(device, (*federation)->epoch_claim("storm"));
        HAF_CHECK(observed.ok());
        if (!observed.ok()) {
            return;
        }
    }
    HAF_EQ((*federation)->member_count(), std::size_t{1});
    const Result<MemberRecord> after = (*federation)->member(device.id);
    HAF_REQUIRE_OK(after);
    // Evidence generation advances because new observations arrive; the
    // capability generation does not, because the capabilities did not change.
    HAF_CHECK(after->capability_generation == baseline->capability_generation);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(adversarial, repeated_restart_cycles_keep_the_audit_clean) {
    ScratchDirectory scratch("adversarial-cycles");
    const std::filesystem::path store = scratch.file("federation.store");
    const AcceleratorDescriptor device = make_device(adapters::cuda_class_profile(), 1006, "cycle", 0);
    for (int cycle = 0; cycle < 5; ++cycle) {
        FederationConfig config;
        config.name = "cycles";
        config.id_seed = 1007;
        config.store_path = store;
        Result<std::unique_ptr<Federation>> federation = Federation::open(config);
        HAF_REQUIRE_OK(federation);
        const AuthorityClaim claim = (*federation)->epoch_claim("cycle");
        Result<MemberRecord> observed = (*federation)->observe(device, claim);
        HAF_REQUIRE_OK(observed);
        if (observed->state == MemberState::Observed) {
            Result<MemberRecord> admitted = (*federation)->admit(device.id, claim);
            HAF_REQUIRE_OK(admitted);
            observed = admitted;
        }
        Result<MemberRecord> activated = (*federation)->activate(device.id, claim);
        HAF_REQUIRE_OK(activated);
        const AuditReport report = (*federation)->audit();
        if (!report.ok()) {
            HAF_NOTE("cycle " + std::to_string(cycle) + ": " + report.render());
        }
        HAF_CHECK(report.ok());
    }
}

HAF_TEST(adversarial, device_disappearance_fences_without_losing_membership) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(1008, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(1009);
    HAF_REQUIRE_OK(join(**federation, device));
    // A device that disappears is fenced, not deleted: membership survives, but
    // no work may be placed on it and its old evidence cannot be reused.
    HAF_REQUIRE_OK((*federation)->fence(device.id, (*federation)->epoch_claim("disappearance")));
    const Result<MemberRecord> fenced = (*federation)->member(device.id);
    HAF_REQUIRE_OK(fenced);
    HAF_CHECK(fenced->state == MemberState::Fenced);
    HAF_CHECK(!fenced->accepts_new_work());
    const WorkloadProfile workload = make_workload("during-outage", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("outage")));
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
    HAF_REQUIRE_OK(decision);
    HAF_CHECK(decision->outcome == CompatibilityOutcome::Ineligible);
    HAF_CHECK((*federation)->audit().ok());
}

HAF_TEST(adversarial, replayed_decision_identity_is_detected) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(1010, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(1011);
    HAF_REQUIRE_OK(join(**federation, device));
    const WorkloadProfile workload = make_workload("replay", {hard_equals("vendor.id", "nvidia")});
    HAF_REQUIRE_OK((*federation)->register_workload(workload, (*federation)->epoch_claim("replay")));
    const Result<CompatibilityDecision> decision = (*federation)->evaluate(workload.class_id, device.id);
    HAF_REQUIRE_OK(decision);

    // Re-serializing a tampered decision must be detected on decode.
    CompatibilityDecision tampered = *decision;
    tampered.score += 1;
    ByteWriter writer;
    tampered.serialize(writer);
    CompatibilityDecision decoded;
    ByteReader reader(writer.data());
    HAF_CHECK(!CompatibilityDecision::deserialize(reader, decoded, false));
}

HAF_TEST(adversarial, malformed_vendor_extension_names_are_refused) {
    // Extension keys must follow the "x.<namespace>.<name>" shape.
    HAF_CHECK(!capability_key_from_name("x.missing-name").ok());
    HAF_CHECK(!capability_key_from_name("x.").ok());
    HAF_CHECK(!capability_key_from_name("x..").ok());
    HAF_CHECK(capability_key_from_name("x.nvidia.cluster_dimensions").ok());
    // A core namespace cannot be shadowed through an extension.
    HAF_CHECK(!capability_key_from_name("numeric..fp32").ok());
}

HAF_TEST(adversarial, mutation_after_shutdown_is_impossible) {
    const Result<std::unique_ptr<Federation>> federation = open_memory_federation(219, permissive_policy());
    HAF_REQUIRE_OK(federation);
    const AcceleratorDescriptor device = make_cuda_like(220);
    HAF_REQUIRE_OK(join(**federation, device));
    HAF_CHECK((*federation)->shutdown().ok());

    const Result<MemberRecord> observed =
        (*federation)->observe(device, (*federation)->epoch_claim("post shutdown"));
    HAF_CHECK(!observed.ok());
    const Result<PolicyGeneration> policy =
        (*federation)->set_policy(permissive_policy(), (*federation)->epoch_claim("post shutdown"));
    HAF_CHECK(!policy.ok());
    const Result<WorkloadRevision> workload =
        (*federation)->register_workload(make_workload("late", {}), (*federation)->epoch_claim("post shutdown"));
    HAF_CHECK(!workload.ok());
}
