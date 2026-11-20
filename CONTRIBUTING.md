# Contributing to `ag-mesh`

Thank you for contributing to `ag-mesh`!

## Development Setup
1. Ensure CMake >= 3.24, a C++20 compiler (GCC 12+, Clang 15+, or MSVC 2022), OpenSSL 3.x development headers, and Ninja are installed.
2. Configure and build with tests and sanitizers enabled:
   ```bash
   cmake --preset linux-asan-ubsan
   cmake --build --preset linux-asan-ubsan --parallel
   ctest --preset linux-asan-ubsan --output-on-failure
   ```

## Engineering Standards
- **Zero Stubs:** Never submit `TODO` placeholders or unimplemented methods on main branch.
- **RAII & Thread Safety:** Avoid raw owning pointers; protect shared state with explicit mutexes or Asio strands.
- **Warnings as Errors:** Code must compile cleanly with `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang) and `/W4 /WX` (MSVC).
- **Formatting:** Run `clang-format` (`.clang-format`) and `clang-tidy` (`.clang-tidy`) before submitting a PR.
- **Tests:** Every bug fix or protocol extension must include unit tests in `tests/unit/` and, where applicable, multi-node verification in `tests/integration/test_cluster_5nodes.cpp`.
