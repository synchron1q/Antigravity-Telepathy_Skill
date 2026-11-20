#include "agmesh/net/protocol_codec.hpp"

#include <nlohmann/json.hpp>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace agmesh::net {

namespace {

void write_be32(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

void write_be16(std::uint8_t* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dst[1] = static_cast<std::uint8_t>(v & 0xFFu);
}

std::uint32_t read_be32(const std::uint8_t* src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24) |
           (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) |
           static_cast<std::uint32_t>(src[3]);
}

std::uint16_t read_be16(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(src[0]) << 8) | static_cast<std::uint16_t>(src[1]));
}

} // namespace

TokenBucketLimiter::TokenBucketLimiter(std::uint64_t kbps_limit)
    : last_refill_(std::chrono::steady_clock::now()) {
    set_limit_kbps(kbps_limit);
}

void TokenBucketLimiter::set_limit_kbps(std::uint64_t kbps_limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_per_sec_ = kbps_limit * 1024ULL / 8ULL; // kbps -> bytes/sec
    burst_capacity_ = static_cast<double>(std::max<std::uint64_t>(bytes_per_sec_, 64ULL * 1024ULL));
    tokens_ = burst_capacity_;
    last_refill_ = std::chrono::steady_clock::now();
}

bool TokenBucketLimiter::consume_or_wait(std::size_t bytes, std::chrono::milliseconds max_wait) {
    if (bytes_per_sec_ == 0) return true; // Unlimited

    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (bytes_per_sec_ == 0) return true;
            const auto now = std::chrono::steady_clock::now();
            const double elapsed_sec = std::chrono::duration<double>(now - last_refill_).count();
            tokens_ = std::min(burst_capacity_, tokens_ + elapsed_sec * static_cast<double>(bytes_per_sec_));
            last_refill_ = now;

            if (tokens_ >= static_cast<double>(bytes)) {
                tokens_ -= static_cast<double>(bytes);
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::uint32_t ProtocolCodec::crc32c(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1u)));
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

std::array<std::uint8_t, kFrameHeaderSize> ProtocolCodec::encode_header(FrameHeader header) {
    std::array<std::uint8_t, kFrameHeaderSize> buf{};
    write_be32(buf.data() + 0, header.magic);
    write_be16(buf.data() + 4, header.version);
    buf[6] = header.flags;
    buf[7] = static_cast<std::uint8_t>(header.type);
    write_be32(buf.data() + 8, header.stream_id);
    write_be32(buf.data() + 12, header.seq_no);
    write_be32(buf.data() + 16, header.payload_len);
    const std::uint32_t crc = crc32c(buf.data(), 20);
    write_be32(buf.data() + 20, crc);
    return buf;
}

std::optional<FrameHeader> ProtocolCodec::decode_header(const std::uint8_t* bytes,
                                                        std::size_t len,
                                                        ProtocolErrorCode& err_out) {
    if (len < kFrameHeaderSize) {
        err_out = ProtocolErrorCode::InvalidMagic;
        return std::nullopt;
    }
    FrameHeader hdr;
    hdr.magic = read_be32(bytes + 0);
    if (hdr.magic != kFrameMagic) {
        err_out = ProtocolErrorCode::InvalidMagic;
        return std::nullopt;
    }
    const std::uint32_t computed_crc = crc32c(bytes, 20);
    hdr.header_crc32c = read_be32(bytes + 20);
    if (computed_crc != hdr.header_crc32c) {
        err_out = ProtocolErrorCode::InvalidMagic;
        return std::nullopt;
    }
    hdr.version = read_be16(bytes + 4);
    if ((hdr.version >> 8) != (kProtocolVersion >> 8)) {
        err_out = ProtocolErrorCode::UnsupportedVersion;
        return std::nullopt;
    }
    hdr.flags = bytes[6];
    hdr.type = static_cast<FrameType>(bytes[7]);
    hdr.stream_id = read_be32(bytes + 8);
    hdr.seq_no = read_be32(bytes + 12);
    hdr.payload_len = read_be32(bytes + 16);
    if (hdr.payload_len > kMaxFramePayloadBytes) {
        err_out = ProtocolErrorCode::QuotaExceeded;
        return std::nullopt;
    }
    return hdr;
}

bool ProtocolCodec::zstd_compress(const std::vector<std::uint8_t>& input,
                                  int level,
                                  std::vector<std::uint8_t>& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    const std::size_t bound = ZSTD_compressBound(input.size());
    output.resize(bound);
    const std::size_t res = ZSTD_compress(output.data(), bound, input.data(), input.size(), level);
    if (ZSTD_isError(res)) {
        output.clear();
        return false;
    }
    output.resize(res);
    return true;
}

bool ProtocolCodec::zstd_decompress(const std::vector<std::uint8_t>& input,
                                    std::size_t max_uncompressed_bytes,
                                    std::vector<std::uint8_t>& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    const unsigned long long frame_size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
        return false;
    }
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (frame_size > max_uncompressed_bytes) {
            return false;
        }
        output.resize(static_cast<std::size_t>(frame_size));
        const std::size_t res = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
        if (ZSTD_isError(res)) return false;
        output.resize(res);
        return true;
    }

    // Streaming fallback if frame size unknown
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return false;
    ZSTD_inBuffer in_buf{input.data(), input.size(), 0};
    std::vector<std::uint8_t> chunk(64 * 1024);
    output.clear();
    while (in_buf.pos < in_buf.size) {
        ZSTD_outBuffer out_buf{chunk.data(), chunk.size(), 0};
        const std::size_t rc = ZSTD_decompressStream(dctx, &out_buf, &in_buf);
        if (ZSTD_isError(rc) || output.size() + out_buf.pos > max_uncompressed_bytes) {
            ZSTD_freeDCtx(dctx);
            output.clear();
            return false;
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(out_buf.pos));
    }
    ZSTD_freeDCtx(dctx);
    return true;
}

bool ProtocolCodec::encode_frame(FrameType type,
                                 std::uint32_t stream_id,
                                 std::uint32_t seq_no,
                                 std::uint8_t extra_flags,
                                 const std::vector<std::uint8_t>& plaintext_payload,
                                 std::size_t zstd_threshold,
                                 int zstd_level,
                                 SessionKeys* keys_or_null,
                                 std::vector<std::uint8_t>& wire_out,
                                 std::size_t max_payload_quota) {
    if (plaintext_payload.size() > max_payload_quota) {
        return false;
    }

    std::uint8_t flags = extra_flags;
    std::vector<std::uint8_t> stage_buf = plaintext_payload;

    if (plaintext_payload.size() >= zstd_threshold && zstd_threshold > 0) {
        std::vector<std::uint8_t> compressed;
        if (zstd_compress(plaintext_payload, zstd_level, compressed) &&
            compressed.size() < plaintext_payload.size()) {
            stage_buf = std::move(compressed);
            flags |= kFlagCompressedZstd;
        }
    }

    if (keys_or_null != nullptr) {
        flags |= kFlagEncryptedAead;
    }

    // Build preliminary header with exact post-encryption payload length so AAD matches wire header.
    const std::size_t final_payload_len = keys_or_null ? (stage_buf.size() + 28) : stage_buf.size();
    if (final_payload_len > kMaxFramePayloadBytes) {
        return false;
    }

    FrameHeader hdr;
    hdr.magic = kFrameMagic;
    hdr.version = kProtocolVersion;
    hdr.flags = flags;
    hdr.type = type;
    hdr.stream_id = stream_id;
    hdr.seq_no = seq_no;
    hdr.payload_len = static_cast<std::uint32_t>(final_payload_len);

    const auto hdr_bytes = encode_header(hdr);
    std::vector<std::uint8_t> final_payload;
    if (keys_or_null != nullptr) {
        std::vector<std::uint8_t> aad(hdr_bytes.begin(), hdr_bytes.end());
        if (!CryptoEngine::aead_encrypt(keys_or_null->tx_key,
                                        ++keys_or_null->tx_nonce_counter,
                                        aad,
                                        stage_buf,
                                        final_payload)) {
            return false;
        }
    } else {
        final_payload = std::move(stage_buf);
    }

    wire_out.clear();
    wire_out.reserve(kFrameHeaderSize + final_payload.size());
    wire_out.insert(wire_out.end(), hdr_bytes.begin(), hdr_bytes.end());
    wire_out.insert(wire_out.end(), final_payload.begin(), final_payload.end());
    return true;
}

bool ProtocolCodec::decode_payload(const FrameHeader& header,
                                   const std::vector<std::uint8_t>& raw_payload,
                                   SessionKeys* keys_or_null,
                                   std::vector<std::uint8_t>& plaintext_out,
                                   ProtocolErrorCode& err_out) {
    std::vector<std::uint8_t> decrypted;
    if ((header.flags & kFlagEncryptedAead) != 0) {
        if (!keys_or_null) {
            err_out = ProtocolErrorCode::DecryptFailed;
            return false;
        }
        const auto hdr_bytes = encode_header(header);
        std::vector<std::uint8_t> aad(hdr_bytes.begin(), hdr_bytes.end());
        if (!CryptoEngine::aead_decrypt(keys_or_null->rx_key, aad, raw_payload, decrypted)) {
            err_out = ProtocolErrorCode::DecryptFailed;
            return false;
        }
    } else {
        decrypted = raw_payload;
    }

    if ((header.flags & kFlagCompressedZstd) != 0) {
        if (!zstd_decompress(decrypted, kMaxFramePayloadBytes, plaintext_out)) {
            err_out = ProtocolErrorCode::DecompressFailed;
            return false;
        }
    } else {
        plaintext_out = std::move(decrypted);
    }
    return true;
}

std::string ProtocolCodec::encode_invite_token(const PairingInviteToken& token) {
    nlohmann::json eps = nlohmann::json::array();
    for (const auto& ep : token.endpoints) {
        eps.push_back({
            {"ip", ep.ip},
            {"port", ep.port},
            {"if", ep.interface_name},
            {"kind", ep.kind},
            {"prio", ep.priority}
        });
    }
    nlohmann::json j = {
        {"v", token.version},
        {"node_id", token.node_id},
        {"fingerprint", token.fingerprint},
        {"endpoints", std::move(eps)},
        {"secret", token.secret}
    };
    const std::string raw = j.dump();
    const std::vector<std::uint8_t> bytes(raw.begin(), raw.end());
    return "agmesh://join/v1/" + CryptoEngine::base64url_encode(bytes);
}

std::optional<PairingInviteToken> ProtocolCodec::decode_invite_token(const std::string& token_uri) {
    const std::string prefix = "agmesh://join/v1/";
    if (token_uri.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const auto decoded_bytes = CryptoEngine::base64url_decode(token_uri.substr(prefix.size()));
    if (!decoded_bytes) {
        return std::nullopt;
    }
    try {
        const std::string json_str(decoded_bytes->begin(), decoded_bytes->end());
        const auto j = nlohmann::json::parse(json_str);
        if (!j.is_object()) return std::nullopt;
        PairingInviteToken tok;
        tok.version = j.value("v", 1);
        if (tok.version != 1) return std::nullopt;
        tok.node_id = j.at("node_id").get<std::string>();
        tok.fingerprint = j.at("fingerprint").get<std::string>();
        tok.secret = j.at("secret").get<std::string>();
        if (tok.node_id.empty() || tok.fingerprint.empty() || tok.secret.empty()) {
            return std::nullopt;
        }
        if (j.contains("endpoints") && j["endpoints"].is_array()) {
            for (const auto& item : j["endpoints"]) {
                EndpointCandidate ep;
                ep.ip = item.value("ip", "");
                ep.port = item.value("port", static_cast<std::uint16_t>(47890));
                ep.interface_name = item.value("if", "");
                ep.kind = item.value("kind", "lan");
                ep.priority = item.value("prio", 50);
                if (!ep.ip.empty() && ep.port > 0) {
                    tok.endpoints.push_back(std::move(ep));
                }
            }
        }
        std::sort(tok.endpoints.begin(), tok.endpoints.end(), [](const EndpointCandidate& a, const EndpointCandidate& b) {
            return a.priority > b.priority;
        });
        return tok;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace agmesh::net
