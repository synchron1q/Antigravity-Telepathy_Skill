#include "agmesh/service/service_manager.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agmesh::service {

namespace {

int exec_system_cmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::filesystem::path systemd_unit_path(bool system_wide) {
    if (system_wide) {
        return "/etc/systemd/system/ag-mesh.service";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "systemd" / "user" / "ag-mesh.service";
}

std::filesystem::path launchd_plist_path() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "Library" / "LaunchAgents" / "io.antigravity.agmesh.plist";
}

} // namespace

std::string ServiceManager::render_systemd_unit(const std::filesystem::path& binary_path,
                                                const std::filesystem::path& config_path) {
    std::ostringstream oss;
    oss << "[Unit]\n"
        << "Description=ag-mesh Multi-Host Connector & Context Sync Daemon for Google Antigravity\n"
        << "After=network-online.target\n"
        << "Wants=network-online.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=" << binary_path.string() << " daemon --config " << config_path.string() << "\n"
        << "ExecReload=/bin/kill -HUP $MAINPID\n"
        << "Restart=always\n"
        << "RestartSec=3\n"
        << "LimitNOFILE=65536\n\n"
        << "[Install]\n"
        << "WantedBy=default.target\n";
    return oss.str();
}

std::string ServiceManager::render_launchd_plist(const std::filesystem::path& binary_path,
                                                 const std::filesystem::path& config_path) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n"
        << "<dict>\n"
        << "  <key>Label</key>\n"
        << "  <string>io.antigravity.agmesh</string>\n"
        << "  <key>ProgramArguments</key>\n"
        << "  <array>\n"
        << "    <string>" << binary_path.string() << "</string>\n"
        << "    <string>daemon</string>\n"
        << "    <string>--config</string>\n"
        << "    <string>" << config_path.string() << "</string>\n"
        << "  </array>\n"
        << "  <key>RunAtLoad</key>\n"
        << "  <true/>\n"
        << "  <key>KeepAlive</key>\n"
        << "  <true/>\n"
        << "</dict>\n"
        << "</plist>\n";
    return oss.str();
}

std::string ServiceManager::render_windows_sc_command(const std::filesystem::path& binary_path,
                                                      const std::filesystem::path& config_path) {
    return "sc.exe create ag-mesh binPath= \"\\\"" + binary_path.string() +
           "\\\" daemon --service --config \\\"" + config_path.string() +
           "\\\"\" start= auto DisplayName= \"Antigravity Mesh Context Sync (ag-mesh)\"";
}

bool ServiceManager::install(const std::filesystem::path& binary_path,
                             const std::filesystem::path& config_path,
                             bool system_wide,
                             std::string& output_msg,
                             std::string& error_out) {
#ifdef _WIN32
    (void)system_wide;
    const std::string cmd = render_windows_sc_command(binary_path, config_path);
    const int rc = exec_system_cmd(cmd);
    if (rc != 0) {
        error_out = "Failed to register Windows Service (run elevated Administrator prompt or use Scheduled Task). Command: " + cmd;
        return false;
    }
    exec_system_cmd("sc.exe description ag-mesh \"Multi-host encrypted context sync and MCP connector for Google Antigravity\"");
    exec_system_cmd("sc.exe start ag-mesh");
    output_msg = "Installed and started Windows Service 'ag-mesh'";
    return true;
#elif defined(__APPLE__)
    (void)system_wide;
    const auto plist = launchd_plist_path();
    std::error_code ec;
    std::filesystem::create_directories(plist.parent_path(), ec);
    {
        std::ofstream ofs(plist, std::ios::trunc);
        if (!ofs) {
            error_out = "Cannot write launchd plist: " + plist.string();
            return false;
        }
        ofs << render_launchd_plist(binary_path, config_path);
    }
    exec_system_cmd(("launchctl unload \"" + plist.string() + "\" 2>/dev/null || true").c_str());
    const int rc = exec_system_cmd("launchctl load -w \"" + plist.string() + "\"");
    if (rc != 0) {
        error_out = "launchctl load failed for " + plist.string();
        return false;
    }
    output_msg = "Installed and loaded launchd agent: " + plist.string();
    return true;
#else
    const auto unit = systemd_unit_path(system_wide);
    std::error_code ec;
    std::filesystem::create_directories(unit.parent_path(), ec);
    {
        std::ofstream ofs(unit, std::ios::trunc);
        if (!ofs) {
            error_out = "Cannot write systemd unit: " + unit.string();
            return false;
        }
        ofs << render_systemd_unit(binary_path, config_path);
    }
    const std::string ctl = system_wide ? "systemctl" : "systemctl --user";
    exec_system_cmd(ctl + " daemon-reload");
    const int rc = exec_system_cmd(ctl + " enable --now ag-mesh.service");
    if (rc != 0) {
        error_out = "Unit written to " + unit.string() + ", but '" + ctl + " enable --now ag-mesh.service' returned " + std::to_string(rc);
        return false;
    }
    output_msg = "Installed and enabled " + ctl + " service at " + unit.string();
    return true;
#endif
}

bool ServiceManager::uninstall(bool system_wide, std::string& output_msg, std::string& error_out) {
#ifdef _WIN32
    (void)system_wide;
    exec_system_cmd("sc.exe stop ag-mesh");
    const int rc = exec_system_cmd("sc.exe delete ag-mesh");
    if (rc != 0) {
        error_out = "Failed to delete Windows Service 'ag-mesh'";
        return false;
    }
    output_msg = "Removed Windows Service 'ag-mesh'";
    return true;
#elif defined(__APPLE__)
    (void)system_wide;
    const auto plist = launchd_plist_path();
    exec_system_cmd("launchctl unload -w \"" + plist.string() + "\"");
    std::error_code ec;
    std::filesystem::remove(plist, ec);
    output_msg = "Unloaded and removed launchd plist: " + plist.string();
    return true;
#else
    const std::string ctl = system_wide ? "systemctl" : "systemctl --user";
    exec_system_cmd(ctl + " disable --now ag-mesh.service");
    const auto unit = systemd_unit_path(system_wide);
    std::error_code ec;
    std::filesystem::remove(unit, ec);
    exec_system_cmd(ctl + " daemon-reload");
    (void)error_out;
    output_msg = "Uninstalled systemd unit: " + unit.string();
    return true;
#endif
}

ServiceStatus ServiceManager::status(bool system_wide) {
    ServiceStatus st;
#ifdef _WIN32
    (void)system_wide;
    st.backend = "windows-service";
    st.unit_path_or_name = "ag-mesh";
    st.installed = (exec_system_cmd("sc.exe query ag-mesh >NUL 2>&1") == 0);
    st.running = st.installed;
    st.detail = st.installed ? "Windows Service registered" : "Not registered";
#elif defined(__APPLE__)
    (void)system_wide;
    st.backend = "launchd";
    const auto plist = launchd_plist_path();
    st.unit_path_or_name = plist.string();
    std::error_code ec;
    st.installed = std::filesystem::exists(plist, ec);
    st.running = (exec_system_cmd("launchctl list | grep -q io.antigravity.agmesh") == 0);
    st.detail = st.installed ? "launchd plist present" : "Not installed";
#else
    st.backend = system_wide ? "systemd-system" : "systemd-user";
    const auto unit = systemd_unit_path(system_wide);
    st.unit_path_or_name = unit.string();
    std::error_code ec;
    st.installed = std::filesystem::exists(unit, ec);
    const std::string ctl = system_wide ? "systemctl" : "systemctl --user";
    st.running = (exec_system_cmd(ctl + " is-active --quiet ag-mesh.service") == 0);
    st.detail = st.running ? "active (running)" : (st.installed ? "installed (inactive)" : "not installed");
#endif
    return st;
}

} // namespace agmesh::service
