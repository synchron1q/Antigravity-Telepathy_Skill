#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::net {

struct NodeKeyPair {
    std::string node_id;
    std::array<std::uint8_t, 32> ed25519_private{};
    std::array<std::uint8_t, 32> ed25519_public{};
    std::string fingerprint_sha256;
    std::string public_key_hex;
};

struct EphemeralX25519 {
    std::array<std::uint8_t, 32> priv{};
    std::array<std::uint8_t, 32> pub{};
};

struct SessionKeys {
    std::array<std::uint8_t, 32> tx_key{};
    std::array<std::uint8_t, 32> rx_key{};
    std::uint64_t tx_nonce_counter{0};
    std::uint64_t rx_nonce_counter{0};
};

// Ed25519 Identity + X25519 ECDH + HKDF-SHA256 + ChaCha20-Poly1305 AEAD Engine.
class CryptoEngine {
public:
    static NodeKeyPair generate_node_keypair(const std::string& node_id);
    static bool save_keypair(const NodeKeyPair& kp, const std::filesystem::path& path, std::string& err);
    static std::optional<NodeKeyPair> load_keypair(const std::filesystem::path& path, std::string& err);

    static std::string compute_fingerprint(const std::array<std::uint8_t, 32>& pubkey);
    static std::string generate_random_psk_hex();

    static EphemeralX25519 generate_x25519_ephemeral();
    static bool compute_x25519_shared_secret(const std::array<std::uint8_t, 32>& my_priv,
                                             const std::array<std::uint8_t, 32>& peer_pub,
                                             std::array<std::uint8_t, 32>& shared_out);

    static bool ed25519_sign(const std::array<std::uint8_t, 32>& priv,
                             const std::vector<std::uint8_t>& message,
                             std::array<std::uint8_t, 64>& sig_out);
    static bool ed25519_verify(const std::array<std::uint8_t, 32>& pub,
                               const std::vector<std::uint8_t>& message,
                               const std::array<std::uint8_t, 64>& sig);

    static std::array<std::uint8_t, 32> hmac_sha256(const std::vector<std::uint8_t>& key,
                                                    const std::vector<std::uint8_t>& data);

    static bool hkdf_sha256(const std::vector<std::uint8_t>& ikm,
                            const std::vector<std::uint8_t>& salt,
                            const std::vector<std::uint8_t>& info,
                            std::vector<std::uint8_t>& out_key,
                            std::size_t out_len);

    // AEAD ChaCha20-Poly1305 seal/open: output = 12-byte nonce || ciphertext || 16-byte tag
    static bool aead_encrypt(const std::array<std::uint8_t, 32>& key,
                             std::uint64_t nonce_counter,
                             const std::vector<std::uint8_t>& aad,
                             const std::vector<std::uint8_t>& plaintext,
                             std::vector<std::uint8_t>& sealed_out);

    static bool aead_decrypt(const std::array<std::uint8_t, 32>& key,
                             const std::vector<std::uint8_t>& aad,
                             const std::vector<std::uint8_t>& sealed_in,
                             std::vector<std::uint8_t>& plaintext_out);

    static std::vector<std::uint8_t> hex_to_bytes(const std::string& hex);
    static std::string bytes_to_hex(const std::uint8_t* data, std::size_t len);
    static std::string base64url_encode(const std::vector<std::uint8_t>& data);
    static std::optional<std::vector<std::uint8_t>> base64url_decode(const std::string& encoded);
    static bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t len);
};

} // namespace agmesh::net
