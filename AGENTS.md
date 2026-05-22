# AGENTS.md

## Project Context
- TermChat is a C++20 terminal instant messaging system with a backend server and an FTXUI client.
- The server uses Linux epoll/Reactor-style networking, Protocol Buffers, ScyllaDB for message storage, and MySQL/sqlpp11 for auth and friend data during the transition.
- The client is a terminal UI under `client/` and can be built separately on macOS with the `macos-debug` preset.
- Prefer the DevContainer for full-stack server work because the server build depends on Linux-oriented tooling and database services.

## Repository Layout
- `server/src/`: backend implementation.
  - `core/`: event loop, epoll, TCP connection, and webserver orchestration.
  - `handler/`: HTTP and protobuf request dispatch.
  - `service/`: auth, friend, message, and push business logic.
  - `dao/`: MySQL and ScyllaDB persistence.
  - `pool/`, `buffer/`, `timer/`, `log/`: shared infrastructure.
- `client/`: FTXUI terminal client and network manager.
- `proto/`: protobuf service and message definitions.
- `sql/`: schema used for generated SQL model headers.
- `tests/`: Python functional tests, Go smoke test, and benchmark scripts.

## Build Commands
- Full debug build, usually inside the DevContainer:
  ```bash
  cmake --preset debug
  cmake --build build/debug
  ```
- Release build:
  ```bash
  cmake --preset release
  cmake --build build/release
  ```
- Profiling build:
  ```bash
  cmake --preset perf
  cmake --build build/relwithdebinfo
  ```
- macOS client-only build:
  ```bash
  cmake --preset macos-debug
  cmake --build --preset macos-debug --target client
  ```
- Docker quick start / CI-like validation:
  ```bash
  docker compose up --build -d
  docker compose logs server
  docker compose down
  ```

## Run Commands
- Server listens on port `1316` by default:
  ```bash
  ./build/debug/server/src/server
  ./build/debug/server/src/server -l 0
  ```
- Client:
  ```bash
  ./build/debug/client/client
  ./build/macos-debug/client/client
  ```

## Testing And Validation
- Functional tests expect a running server:
  ```bash
  python3 tests/test_auth.py [username] [password]
  python3 tests/test_friend.py
  python3 tests/test_message.py
  ```
- Smoke test:
  ```bash
  go mod tidy
  go run tests/smoke.go -addr 127.0.0.1:1316 -n 10000
  ```
- For narrow client-only changes on macOS, build `--target client` with the `macos-debug` preset.
- For server, DAO, protocol, or schema changes, prefer the DevContainer or Docker-based validation.
- The GitHub workflow currently validates `docker compose config`, starts `docker compose up --build -d`, checks that the `mywebserver` container is still running, prints logs, then tears it down.

## Code Generation
- CMake generates C++, Python, and Go protobuf outputs from the explicit list in `proto/CMakeLists.txt`.
- If a `.proto` file is added or removed, update `proto/CMakeLists.txt`; do not rely on a recursive glob.
- CMake generates SQL model headers from `sql/schema.sql` through `scripts/ddl2cpp`.
- After changing `sql/schema.sql`, reconfigure/rebuild so the generated C++ model is refreshed.
- Generated build outputs belong under `build/`; do not commit generated artifacts from `build/`.

## Coding Conventions
- Use C++20.
- Follow `.clang-format` in the repository. It is Google-derived, 4-space indentation, 120-column limit, attached braces, sorted includes, and left-aligned pointers.
- Naming used by the project: PascalCase for classes and functions, `snake_case` for variables.
- Keep server layering intact:
  - protocol parsing and dispatch in `handler/`;
  - business rules in `service/`;
  - persistence in `dao/`;
  - shared networking primitives in `core/`.
- Prefer existing helper types and patterns in nearby files before introducing new abstractions.
- Do not move protocol or storage behavior into UI code.
- Keep client UI changes within the FTXUI component structure under `client/ui/` unless the network state model must change.

## Dependency Notes
- C++ dependencies are managed through `vcpkg.json` feature sets:
  - `client`: FTXUI and protobuf.
  - `server`: sqlpp11, MariaDB connector, jwt-cpp, nlohmann-json, and related server dependencies.
- The server build fetches ScyllaDB `cpp-rs-driver` through CMake `FetchContent`, so network access may be needed on a fresh build.
- `VCPKG_ROOT` must be set for CMake presets that use the vcpkg toolchain.

## Change Discipline
- Keep edits scoped to the requested area and avoid broad refactors unless they are needed for correctness.
- Do not delete or overwrite local user changes. Check `git status --short` before and after substantial edits.
- Avoid committing large runtime artifacts such as `log/`, `perf.data`, `perf.data.old`, `perf.svg`, and build directories.
- When touching tests or benchmarks, preserve the expected server address default of `127.0.0.1:1316` unless the task explicitly changes it.
