# Build Requirements

## Architecture

**Only 64-bit (x64) builds are supported.**

The agent depends on:
- `libx264` compiled for x64 with full SIMD assembly (MMX, SSE2, SSSE3, SSE4, AVX, AVX2, AVX512).
- Windows.Graphics.Capture (WinRT) — requires 64-bit.

32-bit builds will fail at link time (libx264 is x64 only) and would have poor performance.

---

## Windows (Visual Studio 2022/2026)

### Prerequisites

| Tool | Required | Install |
|------|----------|---------|
| Visual Studio 2022+ | Yes | "Desktop development with C++" + "C++/WinRT" workloads |
| CMake 3.15+ | Yes | Via VS installer or `winget install cmake` |
| nasm 2.15+ | Yes | `winget install nasm` |
| GNU make | Yes | GnuWin32 make (`C:\Program Files (x86)\GnuWin32\bin\make.exe`) or MSYS2 make |

### libx264 with SIMD

The project requires `libx264` compiled for **x64 with assembly optimisations** (nasm assembler). Build steps:

```cmd
cd x264_source
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
nmake clean
nmake lib-static
```

**Important:** `config.mak` must contain:
```
AS = "C:/Program Files/NASM/nasm.exe"
ASFLAGS = -f win64 -I. -Icommon/x86/ -DARCH_X86_64=1 -DSTACK_ALIGNMENT=16
```

If using a hand-crafted `config.mak`, ensure `-DARCH_X86_64=1` is present in `ASFLAGS`. Without it, `x86inc.asm` defaults to 32-bit mode and assembly will fail with operand errors.

Output: `libx264.lib` (typically 2+ MB with SIMD; ~1.5 MB without).

Copy to SDK:
```
copy libx264.lib C:\x264_sdk\lib\x64\
copy x264.h       C:\x264_sdk\include\
copy x264_config.h C:\x264_sdk\include\
```

### Building the agent

```cmd
cmake -B build64 -A x64
cmake --build build64 --config Release
```

Output: `build64\Release\desktop_streamer.exe`

### Verifying static linking

Ensure no Visual C++ redistributable DLLs are required:

```cmd
dumpbin /dependents build64\Release\desktop_streamer.exe
```

Expected: only system DLLs (`KERNEL32.dll`, `WS2_32.dll`, `d3d11.dll`, `USER32.dll`, `ole32.dll`, `OLEAUT32.dll`, WinRT API-sets). **No** `MSVCP140.dll`, `VCRUNTIME140.dll`, or `VCRUNTIME140_1.dll`.

### Automated build

Run `build.bat` from a `Developer Command Prompt for VS 2022/2026` (or any `cmd.exe` — the script will find VS automatically):

```cmd
build.bat
```

---

## Linux

The agent (`desktop_streamer.exe`) is Windows-only (uses WGC / WinRT). Linux is relevant for:

1. **Building libx264 for cross-compilation** (optional).
2. **Running the Python server** (`webrtc_server.py`) — see `README.md`.

### Building libx264 natively on Linux

```bash
git clone https://code.videolan.org/videolan/x264.git
cd x264
./configure --enable-static --disable-shared --host=x86_64-linux
make -j$(nproc)
sudo make install
```

- SIMD is auto-detected (yasm/nasm required).
- Static linking: use `--enable-static`.
- To verify SIMD is enabled, check configure output: `ASSEMBLY: yes, MMX: yes, SSE2: yes, ...

### Cross-compilation (x264 for Windows on Linux)

```bash
sudo apt install mingw-w64 nasm
git clone https://code.videolan.org/videolan/x264.git
cd x264
./configure --host=x86_64-w64-mingw32 --enable-static --cross-prefix=x86_64-w64-mingw32-
make -j$(nproc)
```

Produces `libx264.a` (MinGW format). Convert to MSVC `.lib` if needed (or link with MinGW toolchain).

---

## Performance notes

- **SIMD assembly** provides 2×–4× speedup in motion estimation, DCT, pixel operations, and deblocking.
- **`b_sliced_threads=1`** is required for multi-monitor performance and is set by default in `encoder_x264.cpp`.
- The lib should be linked statically (`/MT` / `-static`) to avoid redistributable dependencies.
- Verify SIMD is active at runtime by checking encoder logs for: `using cpu capabilities: MMX2 SSE2Fast SSSE3 Cache64 SlowCTL SlowAtom`
