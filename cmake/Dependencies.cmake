include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Auto-discover OpenSSL root on Windows and macOS when not passed explicitly via OPENSSL_ROOT_DIR.
if(NOT DEFINED OPENSSL_ROOT_DIR)
    if(DEFINED ENV{OPENSSL_ROOT_DIR} AND EXISTS "$ENV{OPENSSL_ROOT_DIR}")
        set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
    elseif(WIN32)
        set(_ossl_candidates
            "C:/Program Files/OpenSSL-Win64"
            "C:/Program Files/OpenSSL"
            "C:/OpenSSL-Win64"
            "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows")
        if(MINGW)
            list(APPEND _ossl_candidates "C:/Program Files/Git/mingw64")
        endif()
        foreach(_ossl_cand IN ITEMS ${_ossl_candidates})
            if(EXISTS "${_ossl_cand}/include/openssl/ssl.h" OR EXISTS "${_ossl_cand}/include/openssl/evp.h")
                set(OPENSSL_ROOT_DIR "${_ossl_cand}")
                break()
            endif()
        endforeach()
    elseif(APPLE)
        foreach(_ossl_cand IN ITEMS
            "/opt/homebrew/opt/openssl@3"
            "/usr/local/opt/openssl@3"
            "/opt/homebrew/opt/openssl")
            if(EXISTS "${_ossl_cand}/include/openssl/evp.h")
                set(OPENSSL_ROOT_DIR "${_ossl_cand}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(AGMESH_USE_VCPKG)
    find_package(asio CONFIG QUIET)
    find_package(unofficial-sqlite3 CONFIG QUIET)
    find_package(zstd CONFIG QUIET)
    find_package(tomlplusplus CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(CLI11 CONFIG QUIET)
    find_package(spdlog CONFIG QUIET)
    find_package(OpenSSL REQUIRED)
    if(AGMESH_BUILD_TUI)
        find_package(ftxui CONFIG QUIET)
    endif()
    if(AGMESH_BUILD_TESTS)
        find_package(Catch2 3 CONFIG QUIET)
    endif()
else()
    find_package(OpenSSL REQUIRED)
endif()

# 1. Standalone Asio (v1.30.2)
if(NOT TARGET asio::asio)
    FetchContent_Declare(
        asio
        URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
        URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
    )
    FetchContent_MakeAvailable(asio)
    add_library(agmesh_asio INTERFACE)
    target_include_directories(agmesh_asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
    add_library(agmesh::asio ALIAS agmesh_asio)
else()
    add_library(agmesh::asio ALIAS asio::asio)
endif()

# 2. SQLite3 Amalgamation 3.46.1 (compiled with FTS5, JSON1, WAL, thread-safe serialized mode)
if(TARGET unofficial::sqlite3::sqlite3)
    add_library(agmesh::sqlite3 ALIAS unofficial::sqlite3::sqlite3)
else()
    FetchContent_Declare(
        sqlite3_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
    )
    FetchContent_MakeAvailable(sqlite3_amalgamation)
    add_library(agmesh_sqlite3 STATIC "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(agmesh_sqlite3 SYSTEM PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")
    target_compile_definitions(agmesh_sqlite3 PUBLIC
        SQLITE_ENABLE_FTS5=1
        SQLITE_ENABLE_JSON1=1
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_URI=1
        SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
        SQLITE_OMIT_LOAD_EXTENSION=1
    )
    # Suppress third-party C amalgamation warnings when parent builds with -Werror / /WX
    if(MSVC)
        target_compile_options(agmesh_sqlite3 PRIVATE /W0)
    else()
        target_compile_options(agmesh_sqlite3 PRIVATE -w)
    endif()
    add_library(agmesh::sqlite3 ALIAS agmesh_sqlite3)
endif()

# 3. Zstandard (v1.5.6)
if(TARGET zstd::libzstd_static)
    add_library(agmesh::zstd ALIAS zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    add_library(agmesh::zstd ALIAS zstd::libzstd_shared)
else()
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
        URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    add_library(agmesh::zstd ALIAS libzstd_static)
endif()

# 4. tomlplusplus (v3.4.0)
if(NOT TARGET tomlplusplus::tomlplusplus)
    FetchContent_Declare(
        tomlplusplus
        URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
        URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()
if(TARGET tomlplusplus::tomlplusplus)
    add_library(agmesh::tomlplusplus ALIAS tomlplusplus::tomlplusplus)
else()
    add_library(agmesh::tomlplusplus ALIAS tomlplusplus_tomlplusplus)
endif()

# 5. nlohmann_json (v3.11.3)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
add_library(agmesh::nlohmann_json ALIAS nlohmann_json::nlohmann_json)

# 6. CLI11 (v2.4.2)
if(NOT TARGET CLI11::CLI11)
    FetchContent_Declare(
        CLI11
        URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
        URL_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a
    )
    FetchContent_MakeAvailable(CLI11)
endif()
add_library(agmesh::cli11 ALIAS CLI11::CLI11)

# 7. spdlog (v1.14.1)
if(NOT TARGET spdlog::spdlog)
    FetchContent_Declare(
        spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
        URL_HASH SHA256=1586508029a7d0670dfcb2d97575dcdc242d3868a259742b69f100801ab4e16b
    )
    FetchContent_MakeAvailable(spdlog)
endif()
add_library(agmesh::spdlog ALIAS spdlog::spdlog)

# 8. OpenSSL (Ed25519 / X25519 / ChaCha20-Poly1305 / SHA-256 / HKDF / HMAC)
add_library(agmesh::openssl ALIAS OpenSSL::Crypto)

# 9. FTXUI (v5.0.0)
if(AGMESH_BUILD_TUI)
    if(NOT (TARGET ftxui::screen AND TARGET ftxui::dom AND TARGET ftxui::component))
        set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            ftxui
            URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
            URL_HASH SHA256=a2991cb222c944aee14397965d9f6b050245da849d8c5da7c72d112de2786b5b
        )
        FetchContent_MakeAvailable(ftxui)
    endif()
    add_library(agmesh_ftxui INTERFACE)
    target_link_libraries(agmesh_ftxui INTERFACE ftxui::screen ftxui::dom ftxui::component)
    add_library(agmesh::ftxui ALIAS agmesh_ftxui)
endif()

# 10. Catch2 (v3.7.1) for unit and 5-node integration tests
if(AGMESH_BUILD_TESTS)
    if(NOT TARGET Catch2::Catch2WithMain)
        FetchContent_Declare(
            Catch2
            URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
            URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
        )
        FetchContent_MakeAvailable(Catch2)
    endif()
    add_library(agmesh::catch2 ALIAS Catch2::Catch2WithMain)
endif()
