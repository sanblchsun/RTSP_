# Dockerfile.builder - Container for cross-compilation of RTSP_ Windows agent
# ============================================
# Includes: mingw-w64, clang, libx264 deps, WinRT SDK generator
# ============================================

FROM python:3.13.13-slim-bookworm

WORKDIR /app

# Install system dependencies for cross-compilation:
# - mingw-w64: cross-compiler for Windows agents
# - clang-19/lld-19: LLVM toolchain
# - nasm: assembler for libx264 SIMD
# - cmake: for building cppwinrt
RUN apt-get update && apt-get install -y --fix-missing --no-install-recommends \
    mingw-w64 \
    clang-19 lld-19 llvm-19-dev \
    build-essential \
    p7zip-full \
    cmake \
    git \
    curl \
    pkg-config \
    nasm \
    && rm -rf /var/lib/apt/lists/* \
    && rm -rf /usr/lib/gcc/x86_64-w64-mingw32/12-win32 \
    && ln -sf 12-posix /usr/lib/gcc/x86_64-w64-mingw32/12-win32

# Keep container running for interactive exec
CMD ["sleep", "infinity"]