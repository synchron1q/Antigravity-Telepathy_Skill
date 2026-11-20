#pragma once

#include <string>
#include <vector>

namespace agmesh::cli {

class CliApplication {
public:
    static int run(int argc, char** argv);

    // Exposed helpers for testing and programmatic orchestration
    static bool install_antigravity_skill_and_mcp(const std::string& binary_path,
                                                  const std::string& config_path,
                                                  std::string& report_out,
                                                  std::string& error_out);
};

} // namespace agmesh::cli
