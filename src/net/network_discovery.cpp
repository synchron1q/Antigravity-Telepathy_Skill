#include "agmesh/net/network_discovery.hpp"

#include <asio/ip/address.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace agmesh::net {

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

bool is_vpn_interface_name(const std::string& lower_if) {
    return starts_with(lower_if, "wg") ||
           contains(lower_if, "wintun") ||
           contains(lower_if, "wireguard") ||
           starts_with(lower_if, "tailscale") ||
           contains(lower_if, "tailscale") ||
           starts_with(lower_if, "zt") ||
           contains(lower_if, "zerotier") ||
           starts_with(lower_if, "tun") ||
           starts_with(lower_if, "tap") ||
           starts_with(lower_if, "utun") ||
           starts_with(lower_if, "ppp");
}

bool is_tailscale_cgnat_v4(const asio::ip::address_v4::bytes_type& b) noexcept {
    // 100.64.0.0/10 -> first byte 100, second byte 64..127
    return b[0] == 100 && (b[1] >= 64 && b[1] <= 127);
}

bool is_rfc1918_v4(const asio::ip::address_v4::bytes_type& b) noexcept {
    if (b[0] == 10) return true;
    if (b[0] == 172 && (b[1] >= 16 && b[1] <= 31)) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    return false;
}

bool is_link_local_v4(const asio::ip::address_v4::bytes_type& b) noexcept {
    return b[0] == 169 && b[1] == 254;
}

bool is_ula_v6(const asio::ip::address_v6::bytes_type& b) noexcept {
    // fc00::/7
    return (b[0] & 0xFEu) == 0xFCu;
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || *wstr == L'\0') return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
#endif

} // namespace

bool NetworkDiscovery::is_virtual_bridge_interface(const std::string& interface_name) {
    const std::string low = to_lower_ascii(interface_name);
    if (low.empty()) return false;
    if (low == "lo" || starts_with(low, "lo:") || contains(low, "loopback")) return true;
    if (starts_with(low, "docker") ||
        starts_with(low, "veth") ||
        starts_with(low, "br-") ||
        starts_with(low, "virbr") ||
        starts_with(low, "cni") ||
        starts_with(low, "flannel") ||
        starts_with(low, "podman")) {
        return true;
    }
    if (starts_with(low, "vethernet") ||
        contains(low, "(wsl") ||
        contains(low, "wsl)") ||
        contains(low, "default switch") ||
        contains(low, "hyper-v")) {
        return true;
    }
    return false;
}

std::optional<EndpointCandidate> NetworkDiscovery::classify_interface_address(
    const std::string& interface_name,
    const std::string& ip,
    std::uint16_t port) {
    if (ip.empty()) return std::nullopt;
    if (is_virtual_bridge_interface(interface_name)) return std::nullopt;

    // Strip IPv6 zone suffix (e.g. %eth0) if present
    std::string clean_ip = ip;
    const auto zone_pos = clean_ip.find('%');
    if (zone_pos != std::string::npos) {
        clean_ip = clean_ip.substr(0, zone_pos);
    }

    std::error_code ec;
    const auto addr = asio::ip::make_address(clean_ip, ec);
    if (ec) return std::nullopt;

    if (addr.is_loopback() || addr.is_unspecified() || addr.is_multicast()) {
        return std::nullopt;
    }

    const std::string low_if = to_lower_ascii(interface_name);
    EndpointCandidate cand;
    cand.ip = clean_ip;
    cand.port = port;
    cand.interface_name = interface_name;

    if (addr.is_v4()) {
        const auto v4 = addr.to_v4();
        const auto b = v4.to_bytes();
        if (is_link_local_v4(b)) return std::nullopt;

        if (is_vpn_interface_name(low_if) || is_tailscale_cgnat_v4(b)) {
            cand.kind = "vpn";
            cand.priority = 100;
        } else if (is_rfc1918_v4(b)) {
            cand.kind = "lan";
            cand.priority = 50;
        } else {
            cand.kind = "public";
            cand.priority = 10;
        }
        return cand;
    }

    const auto v6 = addr.to_v6();
    if (v6.is_link_local() || v6.is_v4_mapped()) {
        return std::nullopt;
    }
    const auto b6 = v6.to_bytes();
    if (is_vpn_interface_name(low_if)) {
        cand.kind = "vpn";
        cand.priority = 95;
    } else if (is_ula_v6(b6)) {
        cand.kind = "lan";
        cand.priority = 45;
    } else {
        cand.kind = "public";
        cand.priority = 5;
    }
    return cand;
}

std::vector<EndpointCandidate> NetworkDiscovery::discover_local_endpoints(std::uint16_t port) {
    std::vector<EndpointCandidate> result;
    std::unordered_set<std::string> seen_ips;

    auto add_candidate = [&](const std::string& if_name, const std::string& ip_str) {
        auto cand = classify_interface_address(if_name, ip_str, port);
        if (!cand) return;
        if (seen_ips.insert(cand->ip).second) {
            result.push_back(std::move(*cand));
        }
    };

#ifdef _WIN32
    ULONG buf_len = 32768;
    std::vector<std::uint8_t> buf(buf_len);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr,
                                    addrs,
                                    &buf_len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr,
                                  addrs,
                                  &buf_len);
    }
    if (rc == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES cur = addrs; cur != nullptr; cur = cur->Next) {
            if (cur->OperStatus != IfOperStatusUp) continue;
            if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            const std::string if_name = wide_to_utf8(cur->FriendlyName);
            const std::string desc_name = wide_to_utf8(cur->Description);
            const std::string combined_if = if_name.empty() ? desc_name : if_name;
            if (is_virtual_bridge_interface(combined_if) || is_virtual_bridge_interface(desc_name)) {
                continue;
            }
            const std::string effective_name = is_vpn_interface_name(to_lower_ascii(desc_name))
                                                   ? desc_name
                                                   : combined_if;

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = cur->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                const sockaddr* sa = ua->Address.lpSockaddr;
                if (!sa) continue;
                char ip_buf[INET6_ADDRSTRLEN]{};
                if (sa->sa_family == AF_INET) {
                    const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
                    if (inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf))) {
                        add_candidate(effective_name, ip_buf);
                    }
                } else if (sa->sa_family == AF_INET6) {
                    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
                    if (inet_ntop(AF_INET6, &sin6->sin6_addr, ip_buf, sizeof(ip_buf))) {
                        add_candidate(effective_name, ip_buf);
                    }
                }
            }
        }
    }
#else
    struct IfAddrsGuard {
        struct ifaddrs* ptr{nullptr};
        ~IfAddrsGuard() {
            if (ptr) freeifaddrs(ptr);
        }
    } guard;

    if (getifaddrs(&guard.ptr) == 0 && guard.ptr != nullptr) {
        for (struct ifaddrs* ifa = guard.ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_name) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
            const std::string if_name(ifa->ifa_name);
            char ip_buf[INET6_ADDRSTRLEN]{};
            if (ifa->ifa_addr->sa_family == AF_INET) {
                const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf))) {
                    add_candidate(if_name, ip_buf);
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(ifa->ifa_addr);
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, ip_buf, sizeof(ip_buf))) {
                    add_candidate(if_name, ip_buf);
                }
            }
        }
    }
#endif

    std::sort(result.begin(), result.end(), [](const EndpointCandidate& a, const EndpointCandidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.ip < b.ip;
    });

    return result;
}

} // namespace agmesh::net
