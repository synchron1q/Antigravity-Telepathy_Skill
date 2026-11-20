#pragma once

#include "agmesh/net/crypto_engine.hpp"
#include "agmesh/net/network_discovery.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::net {

inline constexpr std::uint32_t kFrameMagic = 0x41474D53u; // "AGMS"
inline constexpr std::uint16_t kProtocolVersion = 0x0100u; // v1.0
inline constexpr std::size_t kFrameHeaderSize = 24;
inline constexpr std::size_t kMaxFramePayloadBytes = 16u * 1024u * 1024u;

enum FrameFlags : std::uint8_t {
    kFlagNone = 0x00,
    kFlagCompressedZstd = 0x01,
    kFlagEncryptedAead = 0x02,
    kFlagRelayEnvelope = 0x04,
    kFlagEndOfStream = 0x08,
    kFlagAckRequired = 0x10
};

enum class FrameType : std::uint8_t {
    HandshakeInit = 0x01,
    HandshakeResp = 0x02,
    HandshakeFinish = 0x03,
    HeartbeatPing = 0x04,
    HeartbeatPong = 0x05,
    FrameAck = 0x06,
    FrameError = 0x07,
    PairRequest = 0x08,
    PairWelcome = 0x09,
    PairReject = 0x0A,

    NodeHelloMeta = 0x10,
    SyncClockReq = 0x11,
    SyncClockResp = 0x12,

    DeltaEventBatch = 0x20,
    ArtifactChunk = 0x21,
    HandoffPacket = 0x22,
    LockOp = 0x23,
    PeerGossip = 0x24,
    TombstonePurge = 0x25,

    RelayEnvelope = 0x30
};

enum class ProtocolErrorCode : std::uint16_t {
    InvalidMagic = 0x0001,
    UnsupportedVersion = 0x0002,
    AuthFailed = 0x0003,
    UnauthorizedPeer = 0x0004,
    DecryptFailed = 0x0005,
    DecompressFailed = 0x0006,
    QuotaExceeded = 0x0007,
    RelayTargetOffline = 0x0008,
    RateLimited = 0x0009
};

struct FrameHeader {
    std::uint32_t magic{kFrameMagic};
    std::uint16_t version{kProtocolVersion};
    std::uint8_t flags{0};
    FrameType type{FrameType::HeartbeatPing};
    std::uint32_t stream_id{0};
    std::uint32_t seq_no{0};
    std::uint32_t payload_len{0};
    std::uint32_t header_crc32c{0};
};

struct DecodedFrame {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

struct PairingInviteToken {
    int version{1};
    std::string node_id;
    std::string fingerprint;
    std::vector<EndpointCandidate> endpoints;
    std::string secret;
};

class TokenBucketLimiter {
public:
    explicit TokenBucketLimiter(std::uint64_t kbps_limit = 0);

    void set_limit_kbps(std::uint64_t kbps_limit);
    // Consumes tokens for `bytes` and returns true if immediately allowed, or waits up to max_wait.
    bool consume_or_wait(std::size_t bytes, std::chrono::milliseconds max_wait);

private:
    std::mutex mutex_;
    std::uint64_t bytes_per_sec_{0};
    double tokens_{0.0};
    double burst_capacity_{0.0};
    std::chrono::steady_clock::time_point last_refill_;
};

class ProtocolCodec {
public:
    [[nodiscard]] static std::uint32_t crc32c(const std::uint8_t* data, std::size_t len);

    [[nodiscard]] static std::array<std::uint8_t, kFrameHeaderSize> encode_header(FrameHeader header);
    [[nodiscard]] static std::optional<FrameHeader> decode_header(const std::uint8_t* bytes,
                                                                  std::size_t len,
                                                                  ProtocolErrorCode& err_out);

    static bool zstd_compress(const std::vector<std::uint8_t>& input,
                              int level,
                              std::vector<std::uint8_t>& output);
    static bool zstd_decompress(const std::vector<std::uint8_t>& input,
                                std::size_t max_uncompressed_bytes,
                                std::vector<std::uint8_t>& output);

    // Encodes a full wire frame (optional zstd compression + optional ChaCha20-Poly1305 AEAD).
    static bool encode_frame(FrameType type,
                             std::uint32_t stream_id,
                             std::uint32_t seq_no,
                             std::uint8_t extra_flags,
                             const std::vector<std::uint8_t>& plaintext_payload,
                             std::size_t zstd_threshold,
                             int zstd_level,
                             SessionKeys* keys_or_null,
                             std::vector<std::uint8_t>& wire_out,
                             std::size_t max_payload_quota = kMaxFramePayloadBytes);

    static bool decode_payload(const FrameHeader& header,
                               const std::vector<std::uint8_t>& raw_payload,
                               SessionKeys* keys_or_null,
                               std::vector<std::uint8_t>& plaintext_out,
                               ProtocolErrorCode& err_out);

    // Compact invite token encoding/decoding: agmesh://join/v1/<base64url(json)>
    [[nodiscard]] static std::string encode_invite_token(const PairingInviteToken& token);
    [[nodiscard]] static std::optional<PairingInviteToken> decode_invite_token(const std::string& token_uri);
};

} // namespace agmesh::net
