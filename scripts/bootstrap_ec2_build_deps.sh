#!/usr/bin/env bash
set -euo pipefail

# Bootstrap a fresh Ubuntu/Debian EC2 host for TermChat server builds and
# optional Go-based performance benchmarks.

GO_VERSION="${GO_VERSION:-1.25.10}"
VCPKG_ROOT="${VCPKG_ROOT:-${HOME}/.local/opt/vcpkg}"
RUST_TOOLCHAIN="${RUST_TOOLCHAIN:-nightly}"

if [[ -f /etc/os-release ]]; then
    # shellcheck source=/dev/null
    source /etc/os-release
else
    echo "ERROR: /etc/os-release not found; this script expects Ubuntu/Debian." >&2
    exit 1
fi

case "${ID:-}" in
    ubuntu|debian) ;;
    *)
        echo "ERROR: unsupported OS '${ID:-unknown}'. Use Ubuntu/Debian or adapt this script." >&2
        exit 1
        ;;
esac

if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=()
else
    SUDO=(sudo)
fi

arch="$(uname -m)"
case "${arch}" in
    x86_64|amd64) go_arch="amd64" ;;
    aarch64|arm64) go_arch="arm64" ;;
    *)
        echo "ERROR: unsupported architecture '${arch}' for Go binary install." >&2
        exit 1
        ;;
esac

echo "Installing apt packages..."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y \
    autoconf \
    automake \
    autoconf-archive \
    bison \
    build-essential \
    ca-certificates \
    clang-format \
    cmake \
    curl \
    flex \
    gdb \
    gettext \
    git \
    iproute2 \
    libclang-dev \
    libssl-dev \
    libtool \
    libuv1-dev \
    ninja-build \
    pkg-config \
    procps \
    protobuf-compiler \
    python3 \
    python3-pip \
    python3-pyparsing \
    sysstat \
    tar \
    unzip \
    zip

if ! command -v rustup >/dev/null 2>&1; then
    echo "Installing rustup..."
    curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --default-toolchain "${RUST_TOOLCHAIN}"
fi

export CARGO_HOME="${CARGO_HOME:-${HOME}/.cargo}"
export PATH="${CARGO_HOME}/bin:${PATH}"

echo "Ensuring Rust toolchain '${RUST_TOOLCHAIN}'..."
rustup toolchain install "${RUST_TOOLCHAIN}" --profile minimal
rustup default "${RUST_TOOLCHAIN}"

if ! command -v go >/dev/null 2>&1 || ! go version | grep -q "go${GO_VERSION}"; then
    echo "Installing Go ${GO_VERSION}..."
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "${tmpdir}"' EXIT
    curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${go_arch}.tar.gz" -o "${tmpdir}/go.tar.gz"
    "${SUDO[@]}" rm -rf /usr/local/go
    "${SUDO[@]}" tar -C /usr/local -xzf "${tmpdir}/go.tar.gz"
fi

export PATH="/usr/local/go/bin:${HOME}/go/bin:${PATH}"
export VCPKG_ROOT
export VCPKG_FORCE_SYSTEM_BINARIES=1

echo "Installing protoc-gen-go..."
go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11

if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    echo "Cloning vcpkg into ${VCPKG_ROOT}..."
    mkdir -p "$(dirname "${VCPKG_ROOT}")"
    git clone --filter=tree:0 https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
fi

echo "Bootstrapping vcpkg..."
"${VCPKG_ROOT}/bootstrap-vcpkg.sh"

profile_file="${HOME}/.termchat-build-env"
cat >"${profile_file}" <<EOF
export VCPKG_ROOT=${VCPKG_ROOT}
export VCPKG_FORCE_SYSTEM_BINARIES=1
export CARGO_HOME=${CARGO_HOME}
export RUSTUP_HOME=${RUSTUP_HOME:-${HOME}/.rustup}
export RUSTC=${CARGO_HOME}/bin/rustc
export CARGO=${CARGO_HOME}/bin/cargo
export PATH=/usr/local/go/bin:${HOME}/go/bin:${VCPKG_ROOT}:${CARGO_HOME}/bin:\$PATH
EOF

if ! grep -qF ". ${profile_file}" "${HOME}/.profile" 2>/dev/null; then
    echo ". ${profile_file}" >>"${HOME}/.profile"
fi

# Best-effort perf support. Some EC2 kernels need AWS-specific linux-tools
# packages, so do not fail the whole bootstrap when perf is unavailable.
if ! command -v perf >/dev/null 2>&1; then
    "${SUDO[@]}" apt-get install -y linux-tools-common linux-tools-generic "linux-tools-$(uname -r)" || true
fi

echo
echo "Bootstrap complete. Load the environment in this shell with:"
echo "  source ${profile_file}"
echo
echo "Recommended server build:"
echo "  cmake --preset server-release"
echo "  cmake --build --preset server-release"
echo
echo "Build with Go benchmark protobuf outputs:"
echo "  cmake --preset server-perf"
echo "  cmake --build --preset server-perf"
