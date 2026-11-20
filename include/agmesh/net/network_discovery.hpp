#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agmesh::net {

struct EndpointCandidate {
    std::string ip;
    std::uint16_t port{0};
    std::string interface_name;
    std::string kind; // "vpn" | "lan" | "public"
    int priority{0};
};

class NetworkDiscovery {
public:
    // Enumerates active local OS interfaces (Windows GetAdaptersAddresses / POSIX getifaddrs),
    // filters loopback, link-local, and virtual container/hypervisor bridges, and sorts by priority.
    [[nodiscard]] static std::vector<EndpointCandidate> discover_local_endpoints(std::uint16_t port);

    // Pure classification & filtering helper (exposed for deterministic unit testing).
    [[nodiscard]] static std::optional<EndpointCandidate> classify_interface_address(
        const std::string& interface_name,
        const std::string& ip,
        std::uint16_t port);

    [[nodiscard]] static bool is_virtual_bridge_interface(const std::string& interface_name);
};

} // namespace agmesh::net
