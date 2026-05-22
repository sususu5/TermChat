FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

ARG UBUNTU_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports
ARG UBUNTU_MIRROR_USE_HTTPS=true

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
            python3-pyparsing \
            golang-go && break; \
        if [ "$attempt" = 5 ]; then exit 1; fi; \
        apt-get update; \
        sleep 5; \
    done && \
    rm -rf /var/lib/apt/lists/*

RUN apt-get update && \
    (apt-get install -y --no-install-recommends linux-perf || apt-get install -y --no-install-recommends linux-tools-common linux-tools-generic || true) && \
    rm -rf /var/lib/apt/lists/*

ENV PATH=$PATH:/root/go/bin
RUN go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
RUN go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
RUN go install golang.org/x/tools/gopls@latest
RUN go install github.com/go-delve/delve/cmd/dlv@latest

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
RUN curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --default-toolchain nightly --no-modify-path

RUN pip3 install --break-system-packages protobuf==5.29.5

RUN npm install -g @openai/codex

WORKDIR /opt
RUN git clone --filter=tree:0 https://github.com/microsoft/vcpkg.git
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
RUN ./vcpkg/bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=$VCPKG_ROOT:$PATH

RUN git clone --depth 1 https://github.com/brendangregg/FlameGraph.git /opt/FlameGraph
ENV PATH="/opt/FlameGraph:${PATH}"

WORKDIR /app
COPY . .
