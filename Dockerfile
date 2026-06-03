FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
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
    ca-certificates \
    python3 \
    python3-pip \
    protobuf-compiler \
    python3-pyparsing \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH} \
    RUSTC=/usr/local/cargo/bin/rustc \
    CARGO=/usr/local/cargo/bin/cargo
RUN curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --default-toolchain nightly --no-modify-path

RUN pip3 install --break-system-packages protobuf==5.29.5

WORKDIR /opt
RUN git clone --filter=tree:0 https://github.com/microsoft/vcpkg.git
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
RUN ./vcpkg/bootstrap-vcpkg.sh
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=$VCPKG_ROOT:$PATH

WORKDIR /app
COPY . .

CMD ["/bin/bash"]
