#pragma once

#include <filesystem>
#include <string>

namespace agmesh::service {

struct ServiceStatus {
    bool installed{false};
    bool running{false};
    std::string backend; // systemd-user | systemd-system | windows-service | launchd
    std::string unit_path_or_name;
    std::string detail;
};

class ServiceManager {
public:
    static bool install(const std::filesystem::path& binary_path,
                        const std::filesystem::path& config_path,
                        bool system_wide,
                        std::string& output_msg,
                        std::string& error_out);

    static bool uninstall(bool system_wide, std::string& output_msg, std::string& error_out);
    static ServiceStatus status(bool system_wide);

    // Generates platform service unit manifest text without necessarily registering it.
    static std::string render_systemd_unit(const std::filesystem::path& binary_path,
                                           const std::filesystem::path& config_path);
    static std::string render_launchd_plist(const std::filesystem::path& binary_path,
                                            const std::filesystem::path& config_path);
    static std::string render_windows_sc_command(const std::filesystem::path& binary_path,
                                                 const std::filesystem::path& config_path);
};

} // namespace agmesh::service
