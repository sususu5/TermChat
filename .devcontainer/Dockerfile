FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

ARG UBUNTU_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports
ARG UBUNTU_MIRROR_USE_HTTPS=true
ARG GO_VERSION=1.25.10
ARG GO_LINUX_AMD64_SHA256=42d4f7a32316aa66591eca7e89867256057a4264451aca10570a715b3637ba70
ARG GO_LINUX_ARM64_SHA256=654da1f9b50a5d1c2a85ccf8ed405aa89c06e94d18384628bf186f7712677b08
ARG RUST_TOOLCHAIN=nightly-2026-06-02
ARG CODEX_CLI_VERSION=0.136.0
ARG VCPKG_REF=9849e070ad4c3383890fb1c80ae0c54325e2484a
ARG FLAMEGRAPH_REF=41fee1f99f9276008b7cd112fca19dc3ea84ac32

RUN if [ "$UBUNTU_MIRROR" != "http://ports.ubuntu.com/ubuntu-ports" ]; then \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|$UBUNTU_MIRROR|g" /etc/apt/sources.list.d/ubuntu.sources; \
    fi && \
    printf '%s\n' \
        'Acquire::Retries "5";' \
        'Acquire::http::Timeout "30";' \
        'Acquire::https::Timeout "30";' \
        'Acquire::http::Pipeline-Depth "0";' \
        'Acquire::http::No-Cache "true";' \
        'Acquire::BrokenProxy "true";' \
        'APT::Install-Recommends "false";' \
        'APT::Install-Suggests "false";' \
        > /etc/apt/apt.conf.d/99devcontainer-retries

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    if [ "$UBUNTU_MIRROR_USE_HTTPS" = "true" ]; then \
        sed -i 's|http://|https://|g' /etc/apt/sources.list.d/ubuntu.sources; \
    fi && \
    rm -rf /var/lib/apt/lists/*

RUN apt-get update && \
    for attempt in 1 2 3 4 5; do \
        apt-get install -y --no-install-recommends --fix-missing \
            build-essential \
            cmake \
            git \
            curl \
            iproute2 \
            nodejs \
            npm \
            zip \
            unzip \
            tar \
            pkg-config \
            libuv1-dev \
            libssl-dev \
            gdb \
            clangd \
            clang-format \
            libclang-dev \
            ninja-build \
            python3 \
            python3-pip \
            protobuf-compiler \
            python3-pyparsing && break; \
        if [ "$attempt" = 5 ]; then exit 1; fi; \
        apt-get update; \
        sleep 5; \
    done && \
    rm -rf /var/lib/apt/lists/*

RUN apt-get update && \
    (apt-get install -y --no-install-recommends linux-perf || apt-get install -y --no-install-recommends linux-tools-common linux-tools-generic || true) && \
    rm -rf /var/lib/apt/lists/*

RUN arch="$(dpkg --print-architecture)" && \
    case "$arch" in \
        amd64) go_arch=amd64; go_sha256="${GO_LINUX_AMD64_SHA256}" ;; \
        arm64) go_arch=arm64; go_sha256="${GO_LINUX_ARM64_SHA256}" ;; \
        *) echo "Unsupported Go architecture: $arch" >&2; exit 1 ;; \
    esac && \
    curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${go_arch}.tar.gz" -o /tmp/go.tar.gz && \
    echo "${go_sha256}  /tmp/go.tar.gz" | sha256sum -c - && \
    rm -rf /usr/local/go && \
    tar -C /usr/local -xzf /tmp/go.tar.gz && \
    rm /tmp/go.tar.gz

ENV PATH=/usr/local/go/bin:/root/go/bin:$PATH
ENV GOTOOLCHAIN=local

RUN go version && \
    go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11 && \
    go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.6.2 && \
    go install golang.org/x/tools/gopls@v0.21.1 && \
    go install github.com/go-delve/delve/cmd/dlv@v1.26.3

RUN if command -v perf >/dev/null 2>&1; then \
        true; \
    elif perf_path="$(find /usr/lib/linux-tools -name perf -print -quit 2>/dev/null)" && [ -n "$perf_path" ]; then \
        ln -sf "$perf_path" /usr/bin/perf; \
    else \
        printf '%s\n' '#!/bin/sh' 'echo "perf is unavailable in this devcontainer image." >&2' 'exit 127' > /usr/local/bin/perf && \
        chmod +x /usr/local/bin/perf; \
    fi

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH \
    RUSTC=/usr/local/cargo/bin/rustc \
    CARGO=/usr/local/cargo/bin/cargo
RUN curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --default-toolchain "${RUST_TOOLCHAIN}" --no-modify-path

RUN pip3 install --break-system-packages protobuf==5.29.5

RUN npm install -g "@openai/codex@${CODEX_CLI_VERSION}"

WORKDIR /opt
RUN git clone --filter=tree:0 https://github.com/microsoft/vcpkg.git && \
    git -C vcpkg checkout "${VCPKG_REF}"
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
RUN ./vcpkg/bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=$VCPKG_ROOT:$PATH

RUN git clone https://github.com/brendangregg/FlameGraph.git /opt/FlameGraph && \
    git -C /opt/FlameGraph checkout "${FLAMEGRAPH_REF}"
ENV PATH="/opt/FlameGraph:${PATH}"

WORKDIR /app
COPY . .
