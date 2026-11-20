#include "agmesh/net/crypto_engine.hpp"
#include "agmesh/core/hlc_crdt.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace agmesh::net {

namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct EvpPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* p) const noexcept { EVP_PKEY_CTX_free(p); }
};
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* p) const noexcept { EVP_MD_CTX_free(p); }
};
struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* p) const noexcept { EVP_CIPHER_CTX_free(p); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;

} // namespace

std::string CryptoEngine::bytes_to_hex(const std::uint8_t* data, std::size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return oss.str();
}

std::vector<std::uint8_t> CryptoEngine::hex_to_bytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte_val = 0;
        std::istringstream iss(hex.substr(i, 2));
        iss >> std::hex >> byte_val;
        if (iss.fail()) return {};
        out.push_back(static_cast<std::uint8_t>(byte_val & 0xFFu));
    }
    return out;
}

bool CryptoEngine::constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t len) {
    return CRYPTO_memcmp(a, b, len) == 0;
}

std::string CryptoEngine::base64url_encode(const std::vector<std::uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[v & 0x3Fu]);
        i += 3;
    }
    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3Fu]);
    } else if (rem == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(v >> 6) & 0x3Fu]);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> CryptoEngine::base64url_decode(const std::string& encoded) {
    if (encoded.empty()) return std::vector<std::uint8_t>{};
    std::array<int, 256> lut{};
    lut.fill(-1);
    const std::string alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int idx = 0; idx < 64; ++idx) {
        lut[static_cast<unsigned char>(alpha[static_cast<std::size_t>(idx)])] = idx;
    }
    lut[static_cast<unsigned char>('+')] = 62;
    lut[static_cast<unsigned char>('/')] = 63;

    std::vector<std::uint8_t> out;
    out.reserve((encoded.size() * 3) / 4);
    std::uint32_t val = 0;
    int valb = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        const int d = lut[c];
        if (d == -1) return std::nullopt;
        val = (val << 6) + static_cast<std::uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFFu));
            valb -= 8;
        }
    }
    return out;
}

std::string CryptoEngine::compute_fingerprint(const std::array<std::uint8_t, 32>& pubkey) {
    const std::string raw(reinterpret_cast<const char*>(pubkey.data()), pubkey.size());
    return core::CrdtResolver::sha256_hex(raw);
}

std::string CryptoEngine::generate_random_psk_hex() {
    std::array<std::uint8_t, 32> buf{};
    RAND_bytes(buf.data(), static_cast<int>(buf.size()));
    return bytes_to_hex(buf.data(), buf.size());
}

NodeKeyPair CryptoEngine::generate_node_keypair(const std::string& node_id) {
    NodeKeyPair kp;
    kp.node_id = node_id;

    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr));
    EVP_PKEY* raw_pkey = nullptr;
    if (kctx && EVP_PKEY_keygen_init(kctx.get()) == 1 && EVP_PKEY_keygen(kctx.get(), &raw_pkey) == 1) {
        PkeyPtr pkey(raw_pkey);
        std::size_t priv_len = kp.ed25519_private.size();
        std::size_t pub_len = kp.ed25519_public.size();
        EVP_PKEY_get_raw_private_key(pkey.get(), kp.ed25519_private.data(), &priv_len);
        EVP_PKEY_get_raw_public_key(pkey.get(), kp.ed25519_public.data(), &pub_len);
    }

    kp.public_key_hex = bytes_to_hex(kp.ed25519_public.data(), kp.ed25519_public.size());
    kp.fingerprint_sha256 = compute_fingerprint(kp.ed25519_public);
    return kp;
}

bool CryptoEngine::save_keypair(const NodeKeyPair& kp, const std::filesystem::path& path, std::string& err) {
    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        nlohmann::json j = {
            {"node_id", kp.node_id},
            {"algorithm", "ed25519"},
            {"private_key_hex", bytes_to_hex(kp.ed25519_private.data(), kp.ed25519_private.size())},
            {"public_key_hex", kp.public_key_hex},
            {"fingerprint_sha256", kp.fingerprint_sha256}
        };
        const std::string tmp = path.string() + ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                err = "Cannot open key file for writing: " + tmp;
                return false;
            }
            ofs << j.dump(2) << "\n";
            ofs.flush();
            if (!ofs) {
                err = "Failed to flush key file: " + tmp;
                return false;
            }
        }
        std::filesystem::rename(tmp, path);
#ifndef _WIN32
        std::error_code ec;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ec);
#endif
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

std::optional<NodeKeyPair> CryptoEngine::load_keypair(const std::filesystem::path& path, std::string& err) {
    try {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            err = "Key file does not exist: " + path.string();
            return std::nullopt;
        }
        const auto j = nlohmann::json::parse(ifs);
        NodeKeyPair kp;
        kp.node_id = j.value("node_id", "");
        const auto priv_bytes = hex_to_bytes(j.value("private_key_hex", ""));
        const auto pub_bytes = hex_to_bytes(j.value("public_key_hex", ""));
        if (priv_bytes.size() != 32 || pub_bytes.size() != 32) {
            err = "Invalid Ed25519 key length in key file";
            return std::nullopt;
        }
        std::copy(priv_bytes.begin(), priv_bytes.end(), kp.ed25519_private.begin());
        std::copy(pub_bytes.begin(), pub_bytes.end(), kp.ed25519_public.begin());
        kp.public_key_hex = bytes_to_hex(kp.ed25519_public.data(), kp.ed25519_public.size());
        kp.fingerprint_sha256 = compute_fingerprint(kp.ed25519_public);
        return kp;
    } catch (const std::exception& ex) {
        err = ex.what();
        return std::nullopt;
    }
}

EphemeralX25519 CryptoEngine::generate_x25519_ephemeral() {
    EphemeralX25519 eph;
    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr));
    EVP_PKEY* raw = nullptr;
    if (kctx && EVP_PKEY_keygen_init(kctx.get()) == 1 && EVP_PKEY_keygen(kctx.get(), &raw) == 1) {
        PkeyPtr pkey(raw);
        std::size_t priv_len = eph.priv.size();
        std::size_t pub_len = eph.pub.size();
        EVP_PKEY_get_raw_private_key(pkey.get(), eph.priv.data(), &priv_len);
        EVP_PKEY_get_raw_public_key(pkey.get(), eph.pub.data(), &pub_len);
    }
    return eph;
}

bool CryptoEngine::compute_x25519_shared_secret(const std::array<std::uint8_t, 32>& my_priv,
                                                const std::array<std::uint8_t, 32>& peer_pub,
                                                std::array<std::uint8_t, 32>& shared_out) {
    PkeyPtr priv_key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, my_priv.data(), my_priv.size()));
    PkeyPtr pub_key(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_pub.data(), peer_pub.size()));
    if (!priv_key || !pub_key) return false;

    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(priv_key.get(), nullptr));
    if (!ctx) return false;
    if (EVP_PKEY_derive_init(ctx.get()) != 1) return false;
    if (EVP_PKEY_derive_set_peer(ctx.get(), pub_key.get()) != 1) return false;

    std::size_t len = shared_out.size();
    return EVP_PKEY_derive(ctx.get(), shared_out.data(), &len) == 1 && len == shared_out.size();
}

bool CryptoEngine::ed25519_sign(const std::array<std::uint8_t, 32>& priv,
                                const std::vector<std::uint8_t>& message,
                                std::array<std::uint8_t, 64>& sig_out) {
    PkeyPtr pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), priv.size()));
    if (!pkey) return false;

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return false;
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) return false;

    std::size_t sig_len = sig_out.size();
    return EVP_DigestSign(ctx.get(), sig_out.data(), &sig_len, message.data(), message.size()) == 1 &&
           sig_len == sig_out.size();
}

bool CryptoEngine::ed25519_verify(const std::array<std::uint8_t, 32>& pub,
                                  const std::vector<std::uint8_t>& message,
                                  const std::array<std::uint8_t, 64>& sig) {
    PkeyPtr pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size()));
    if (!pkey) return false;

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return false;
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) return false;
    return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), message.data(), message.size()) == 1;
}

std::array<std::uint8_t, 32> CryptoEngine::hmac_sha256(const std::vector<std::uint8_t>& key,
                                                       const std::vector<std::uint8_t>& data) {
    std::array<std::uint8_t, 32> mac{};
    const void* key_ptr = key.empty() ? reinterpret_cast<const void*>("") : key.data();
    const unsigned char* data_ptr = data.empty() ? reinterpret_cast<const unsigned char*>("") : data.data();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
    std::size_t out_len = 0;
    EVP_Q_mac(nullptr,
              "HMAC",
              nullptr,
              "SHA256",
              nullptr,
              key_ptr,
              key.size(),
              data_ptr,
              data.size(),
              mac.data(),
              mac.size(),
              &out_len);
#else
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(),
         key_ptr,
         static_cast<int>(key.size()),
         data_ptr,
         data.size(),
         mac.data(),
         &mac_len);
#endif
    return mac;
}

bool CryptoEngine::hkdf_sha256(const std::vector<std::uint8_t>& ikm,
                               const std::vector<std::uint8_t>& salt,
                               const std::vector<std::uint8_t>& info,
                               std::vector<std::uint8_t>& out_key,
                               std::size_t out_len) {
    out_key.resize(out_len);
    PkeyCtxPtr pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
    if (!pctx) return false;
    if (EVP_PKEY_derive_init(pctx.get()) <= 0) return false;
    if (EVP_PKEY_CTX_set_hkdf_md(pctx.get(), EVP_sha256()) <= 0) return false;
    if (!salt.empty()) {
        if (EVP_PKEY_CTX_set1_hkdf_salt(pctx.get(), salt.data(), static_cast<int>(salt.size())) <= 0) return false;
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx.get(), ikm.data(), static_cast<int>(ikm.size())) <= 0) return false;
    if (!info.empty()) {
        if (EVP_PKEY_CTX_add1_hkdf_info(pctx.get(), info.data(), static_cast<int>(info.size())) <= 0) return false;
    }
    std::size_t actual_len = out_len;
    if (EVP_PKEY_derive(pctx.get(), out_key.data(), &actual_len) <= 0) return false;
    out_key.resize(actual_len);
    return actual_len == out_len;
}

bool CryptoEngine::aead_encrypt(const std::array<std::uint8_t, 32>& key,
                                std::uint64_t nonce_counter,
                                const std::vector<std::uint8_t>& aad,
                                const std::vector<std::uint8_t>& plaintext,
                                std::vector<std::uint8_t>& sealed_out) {
    std::array<std::uint8_t, 12> nonce{};
    // 4 bytes random prefix + 8 bytes counter guarantees uniqueness without wrap collisions
    RAND_bytes(nonce.data(), 4);
    for (int i = 0; i < 8; ++i) {
        nonce[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((nonce_counter >> (56 - i * 8)) & 0xFFu);
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1) {
        return false;
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) return false;

    int len = 0;
    if (!aad.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) return false;
    }

    sealed_out.resize(12 + plaintext.size() + 16);
    std::copy(nonce.begin(), nonce.end(), sealed_out.begin());

    int cipher_len = 0;
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(ctx.get(),
                              sealed_out.data() + 12,
                              &cipher_len,
                              plaintext.data(),
                              static_cast<int>(plaintext.size())) != 1) {
            return false;
        }
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), sealed_out.data() + 12 + cipher_len, &final_len) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16, sealed_out.data() + 12 + cipher_len + final_len) != 1) {
        return false;
    }
    sealed_out.resize(12 + static_cast<std::size_t>(cipher_len + final_len) + 16);
    return true;
}

bool CryptoEngine::aead_decrypt(const std::array<std::uint8_t, 32>& key,
                                const std::vector<std::uint8_t>& aad,
                                const std::vector<std::uint8_t>& sealed_in,
                                std::vector<std::uint8_t>& plaintext_out) {
    if (sealed_in.size() < 28) return false; // 12-byte nonce + 16-byte tag
    const std::uint8_t* nonce = sealed_in.data();
    const std::uint8_t* ciphertext = sealed_in.data() + 12;
    const std::size_t cipher_len = sealed_in.size() - 28;
    const std::uint8_t* tag = sealed_in.data() + 12 + cipher_len;

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) return false;
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce) != 1) return false;

    int len = 0;
    if (!aad.empty()) {
        if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) return false;
    }

    plaintext_out.resize(cipher_len);
    int out_len = 0;
    if (cipher_len > 0) {
        if (EVP_DecryptUpdate(ctx.get(), plaintext_out.data(), &out_len, ciphertext, static_cast<int>(cipher_len)) != 1) {
            return false;
        }
    }

    std::array<std::uint8_t, 16> mutable_tag{};
    std::copy_n(tag, 16, mutable_tag.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16, mutable_tag.data()) != 1) return false;

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext_out.data() + out_len, &final_len) != 1) {
        plaintext_out.clear();
        return false;
    }
    plaintext_out.resize(static_cast<std::size_t>(out_len + final_len));
    return true;
}

} // namespace agmesh::net
