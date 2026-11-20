cmake_minimum_required(VERSION 3.24)

project(ag-mesh
    VERSION 0.1.0
    DESCRIPTION "Distributed multi-host connector, E2EE context sync, and MCP server for Google Antigravity"
    HOMEPAGE_URL "https://github.com/synchron1q/Antigravity-Telepathy_Skill"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AGMESH_BUILD_TESTS "Build unit and integration tests" ON)
option(AGMESH_BUILD_TUI "Build FTXUI terminal user interface" ON)
option(AGMESH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(AGMESH_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AGMESH_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(AGMESH_USE_VCPKG "Prefer find_package for dependencies provided by vcpkg" OFF)
option(AGMESH_STATIC_LINK "Attempt static linking for portable single-binary releases" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

# Core library containing all ag-mesh subsystems (shared by CLI binary and test targets)
add_library(agmesh_core STATIC
    src/core/config.cpp
    src/core/hlc_crdt.cpp
    src/sanitizer/secret_sanitizer.cpp
    src/storage/sqlite_store.cpp
    src/adapter/file_watcher.cpp
    src/adapter/antigravity_adapter.cpp
    src/net/crypto_engine.cpp
    src/net/network_discovery.cpp
    src/net/protocol_codec.cpp
    src/net/mesh_transport.cpp
    src/ipc/local_ipc.cpp
    src/mcp/mcp_server.cpp
    src/tui/dashboard_tui.cpp
    src/service/service_manager.cpp
    src/cli/cli_commands.cpp
)

add_library(agmesh::core ALIAS agmesh_core)

target_include_directories(agmesh_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(agmesh_core PUBLIC cxx_std_20)

agmesh_apply_compiler_warnings(agmesh_core)
agmesh_apply_sanitizers(agmesh_core)

target_compile_definitions(agmesh_core
    PUBLIC
        AGMESH_VERSION="${PROJECT_VERSION}"
        ASIO_STANDALONE=1
        ASIO_NO_DEPRECATED=1
        $<$<BOOL:${AGMESH_BUILD_TUI}>:AGMESH_HAS_FTXUI=1>
        $<$<NOT:$<BOOL:${AGMESH_BUILD_TUI}>>:AGMESH_HAS_FTXUI=0>
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
)

target_link_libraries(agmesh_core
    PUBLIC
        agmesh::asio
        agmesh::sqlite3
        agmesh::zstd
        agmesh::tomlplusplus
        agmesh::nlohmann_json
        agmesh::cli11
        agmesh::spdlog
        agmesh::openssl
        $<$<BOOL:${AGMESH_BUILD_TUI}>:agmesh::ftxui>
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:crypt32>
        $<$<PLATFORM_ID:Windows>:advapi32>
        $<$<PLATFORM_ID:Windows>:iphlpapi>
        $<$<PLATFORM_ID:Linux>:pthread>
        $<$<PLATFORM_ID:Linux>:dl>
)

if(APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    target_link_libraries(agmesh_core PUBLIC ${COREFOUNDATION_FRAMEWORK} ${CORESERVICES_FRAMEWORK})
endif()

add_executable(ag-mesh
    src/main.cpp
)

target_link_libraries(ag-mesh PRIVATE agmesh::core)
agmesh_apply_compiler_warnings(ag-mesh)
agmesh_apply_sanitizers(ag-mesh)

if(AGMESH_STATIC_LINK)
    if(MSVC)
        set_property(TARGET ag-mesh PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set_property(TARGET agmesh_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(UNIX AND NOT APPLE)
        target_link_options(ag-mesh PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS ag-mesh RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY skill/antigravity-mesh DESTINATION ${CMAKE_INSTALL_DATADIR}/ag-mesh/skill)

if(AGMESH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
