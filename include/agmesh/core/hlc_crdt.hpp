#pragma once

#include "agmesh/core/types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agmesh::core {

enum class ClockRelation {
    Equal,
    Before,
    After,
    Concurrent
};

class HybridLogicalClock {
public:
    explicit HybridLogicalClock(std::string node_id);

    // Advances clock for a local state mutation.
    [[nodiscard]] HlcTimestamp tick();

    // Merges remote timestamp upon packet arrival to preserve causal order.
    [[nodiscard]] HlcTimestamp witness(const HlcTimestamp& remote);

    [[nodiscard]] HlcTimestamp current() const;
    void set_node_id(std::string node_id);

private:
    mutable std::mutex mutex_;
    std::string node_id_;
    std::uint64_t last_physical_ms_{0};
    std::uint32_t last_logical_{0};
};

class VectorClockOps {
public:
    static std::uint64_t increment(VectorClock& vc, const std::string& node_id);
    static void merge(VectorClock& target, const VectorClock& remote);
    [[nodiscard]] static ClockRelation compare(const VectorClock& lhs, const VectorClock& rhs);
    [[nodiscard]] static std::string to_json(const VectorClock& vc);
    [[nodiscard]] static VectorClock from_json(const std::string& json_str);
};

// Deterministic CRDT conflict resolution helpers (LWW-Element-Set + Append-Only Log).
class CrdtResolver {
public:
    template <typename T>
    [[nodiscard]] static bool should_accept_lww(const T& existing, const T& incoming) {
        return incoming.hlc > existing.hlc;
    }

    template <typename T>
    static bool merge_lww(T& target, const T& incoming) {
        if (incoming.hlc > target.hlc) {
            target = incoming;
            return true;
        }
        return false;
    }

    // Computes canonical SHA-256 hex digest of UTF-8 input bytes.
    [[nodiscard]] static std::string sha256_hex(const std::string& data);

    // Computes deterministic SHA-256 for chat messages (Append-Only Log deduplication).
    [[nodiscard]] static std::string compute_message_hash(const std::string& session_id,
                                                            const std::string& role,
                                                            std::int64_t created_at_ms,
                                                            const std::string& content);
};

} // namespace agmesh::core
