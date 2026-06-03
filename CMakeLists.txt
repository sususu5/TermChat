cmake_minimum_required(VERSION 3.21)

# Top-level project declaration
project(TermChat)

# ---------------------------------------------------------------------------
# Global build options
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # For libraries that might be linked statically or shared

# Export compile_commands.json for IDEs (VSCode, clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build switches. Server-only presets disable client and non-C++ proto outputs to
# keep EC2 deployments from requiring UI and test-tooling dependencies.
option(BUILD_SERVER "Build the server application" ON)
option(BUILD_CLIENT "Build the terminal client application" ON)
option(BUILD_PROTO_PYTHON "Generate Python protobuf files for tests/tools" ON)
option(BUILD_PROTO_GO "Generate Go protobuf files for benchmarks/smoke tests" ON)

# Optionally select build type from command line, default to Release if none given
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# ---------------------------------------------------------------------------
# vcpkg dependencies
# ---------------------------------------------------------------------------
find_package(protobuf CONFIG REQUIRED)
message(STATUS "Found Protobuf: ${protobuf_VERSION}")

# ---------------------------------------------------------------------------
# Rust toolchain (for cpp-rs-driver) - Only needed for server
# ---------------------------------------------------------------------------
if(BUILD_SERVER)
    if(DEFINED CMAKE_Rust_COMPILER AND CMAKE_Rust_COMPILER AND NOT EXISTS "${CMAKE_Rust_COMPILER}")
        unset(CMAKE_Rust_COMPILER CACHE)
    endif()
    if(DEFINED CARGO_EXECUTABLE AND CARGO_EXECUTABLE AND NOT EXISTS "${CARGO_EXECUTABLE}")
        unset(CARGO_EXECUTABLE CACHE)
    endif()

    find_program(CMAKE_Rust_COMPILER
        NAMES rustc
        HINTS "$ENV{CARGO_HOME}" "$ENV{HOME}/.cargo" "/usr/local/cargo"
        PATH_SUFFIXES bin
        REQUIRED
    )
    find_program(CARGO_EXECUTABLE
        NAMES cargo
        HINTS "$ENV{CARGO_HOME}" "$ENV{HOME}/.cargo" "/usr/local/cargo"
        PATH_SUFFIXES bin
        REQUIRED
    )

    get_filename_component(_termchat_cargo_bin "${CARGO_EXECUTABLE}" DIRECTORY)
    get_filename_component(_termchat_cargo_home "${_termchat_cargo_bin}" DIRECTORY)
    set(CARGO_HOME "${_termchat_cargo_home}" CACHE PATH "Rust Cargo home used by cpp-rs-driver")
    set(ENV{CARGO_HOME} "${CARGO_HOME}")
    set(ENV{RUSTC} "${CMAKE_Rust_COMPILER}")
    set(ENV{CARGO} "${CARGO_EXECUTABLE}")
    set(ENV{PATH} "${_termchat_cargo_bin}:$ENV{PATH}")

    message(STATUS "Using rustc: ${CMAKE_Rust_COMPILER}")
    message(STATUS "Using cargo: ${CARGO_EXECUTABLE}")

    # ---------------------------------------------------------------------------
    # ScyllaDB C++ driver (cpp-rs-driver)
    # ---------------------------------------------------------------------------
    include(FetchContent)

    set(CASS_BUILD_STATIC OFF CACHE BOOL "Disable static build for cpp-rs-driver" FORCE)
    set(CASS_BUILD_SHARED ON CACHE BOOL "Enable shared build for cpp-rs-driver" FORCE)
    set(CASS_BUILD_EXAMPLES OFF CACHE BOOL "Disable cpp-rs-driver examples" FORCE)
    set(CASS_BUILD_INTEGRATION_TESTS OFF CACHE BOOL "Disable cpp-rs-driver tests" FORCE)

    FetchContent_Declare(
        cpp_rs_driver
        GIT_REPOSITORY https://github.com/scylladb/cpp-rs-driver.git
        GIT_TAG v0.6.0
    )
    FetchContent_MakeAvailable(cpp_rs_driver)
endif()

# ---------------------------------------------------------------------------
# Sub-directories
# ---------------------------------------------------------------------------
add_subdirectory(proto)

if(BUILD_SERVER)
    add_subdirectory(server/src)
endif()

if(BUILD_CLIENT)
    add_subdirectory(client)
endif()

# ---------------------------------------------------------------------------
# Packaging / installation rules (optional)
# ---------------------------------------------------------------------------
# install(TARGETS server RUNTIME DESTINATION bin)
