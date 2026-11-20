#include "agmesh/core/hlc_crdt.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace agmesh::core {

HybridLogicalClock::HybridLogicalClock(std::string node_id)
    : node_id_(std::move(node_id)) {}

HlcTimestamp HybridLogicalClock::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto pt = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ms()));
    if (pt > last_physical_ms_) {
        last_physical_ms_ = pt;
        last_logical_ = 0;
    } else {
        ++last_logical_;
    }
    return HlcTimestamp{last_physical_ms_, last_logical_, node_id_};
}

HlcTimestamp HybridLogicalClock::witness(const HlcTimestamp& remote) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto pt = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ms()));
    const auto old_pt = last_physical_ms_;
    const auto max_pt = std::max({old_pt, remote.physical_ms, pt});

    if (max_pt == old_pt && max_pt == remote.physical_ms) {
        last_logical_ = std::max(last_logical_, remote.logical) + 1;
    } else if (max_pt == old_pt) {
        ++last_logical_;
    } else if (max_pt == remote.physical_ms) {
        last_logical_ = remote.logical + 1;
    } else {
        last_logical_ = 0;
    }
    last_physical_ms_ = max_pt;
    return HlcTimestamp{last_physical_ms_, last_logical_, node_id_};
}

HlcTimestamp HybridLogicalClock::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return HlcTimestamp{last_physical_ms_, last_logical_, node_id_};
}

void HybridLogicalClock::set_node_id(std::string node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_id_ = std::move(node_id);
}

std::uint64_t VectorClockOps::increment(VectorClock& vc, const std::string& node_id) {
    auto& counter = vc[node_id];
    ++counter;
    return counter;
}

void VectorClockOps::merge(VectorClock& target, const VectorClock& remote) {
    for (const auto& [node, val] : remote) {
        auto it = target.find(node);
        if (it == target.end()) {
            target.emplace(node, val);
        } else {
            it->second = std::max(it->second, val);
        }
    }
}

ClockRelation VectorClockOps::compare(const VectorClock& lhs, const VectorClock& rhs) {
    bool lhs_less = false;
    bool rhs_less = false;

    auto it1 = lhs.begin();
    auto it2 = rhs.begin();
    while (it1 != lhs.end() || it2 != rhs.end()) {
        std::uint64_t v1 = 0;
        std::uint64_t v2 = 0;
        if (it1 != lhs.end() && (it2 == rhs.end() || it1->first < it2->first)) {
            v1 = it1->second;
            ++it1;
        } else if (it2 != rhs.end() && (it1 == lhs.end() || it2->first < it1->first)) {
            v2 = it2->second;
            ++it2;
        } else {
            v1 = it1->second;
            v2 = it2->second;
            ++it1;
            ++it2;
        }
        if (v1 < v2) lhs_less = true;
        if (v2 < v1) rhs_less = true;
    }

    if (!lhs_less && !rhs_less) return ClockRelation::Equal;
    if (lhs_less && !rhs_less) return ClockRelation::Before;
    if (!lhs_less && rhs_less) return ClockRelation::After;
    return ClockRelation::Concurrent;
}

std::string VectorClockOps::to_json(const VectorClock& vc) {
    nlohmann::json j = vc;
    return j.dump();
}

VectorClock VectorClockOps::from_json(const std::string& json_str) {
    VectorClock vc;
    if (json_str.empty()) return vc;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_number_unsigned() || it.value().is_number_integer()) {
                    vc[it.key()] = it.value().get<std::uint64_t>();
                }
            }
        }
    } catch (...) {
        // Malformed vector clock JSON gracefully returns empty clock.
    }
    return vc;
}

std::string CrdtResolver::sha256_hex(const std::string& data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return {};
    }
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
                    EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return oss.str();
}

std::string CrdtResolver::compute_message_hash(const std::string& session_id,
                                               const std::string& role,
                                               std::int64_t created_at_ms,
                                               const std::string& content) {
    std::string canonical;
    canonical.reserve(session_id.size() + role.size() + content.size() + 32);
    canonical.append(session_id);
    canonical.push_back('\x1F');
    canonical.append(role);
    canonical.push_back('\x1F');
    canonical.append(std::to_string(created_at_ms));
    canonical.push_back('\x1F');
    canonical.append(content);
    return sha256_hex(canonical);
}

} // namespace agmesh::core
