# 32-bit x86 toolchain for building/testing the self-contained x86 decompressor.
# Uses linux/386 directly so no multilib is needed (gcc/nasm are native 32-bit).
FROM --platform=linux/386 i386/debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# Work around the qemu-emulation "invalid signature" apt bug on non-x86 hosts.
RUN printf 'Acquire::http::Pipeline-Depth 0;\nAcquire::http::No-Cache true;\nAcquire::BrokenProxy true;\n' \
    > /etc/apt/apt.conf.d/99fixbadproxy && \
    apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    nasm \
    binutils \
    libc6-dev \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

CMD ["/bin/bash"]
