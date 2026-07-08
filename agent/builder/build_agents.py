#!/usr/bin/env python3
"""
build_agents.py — сборка Windows-агента RTSP_ под Linux (кросс-компиляция).

Использует:
  - clang++-19 --target=x86_64-w64-windows-gnu
  - libx264 (статическая, cross-compiled)
  - C++/WinRT headers (сгенерированные cppwinrt)

Запуск:
  python build_agents.py                    # авто-слот (1.0.0, 1.0.1, ...)
  python build_agents.py --slug 2.5.0       # явный слот
  python build_agents.py --server-url https://example.com
"""

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
AGENT_DIR = PROJECT_ROOT / "agent" / "cmd" / "agent"
DIST_DIR = PROJECT_ROOT / "dist" / "agents"
DEPS_BASE = PROJECT_ROOT / "agent" / "builder" / "build_deps_env"
DEPS_INCLUDE = DEPS_BASE / "cross_install" / "include"
DEPS_LIB = DEPS_BASE / "cross_install" / "lib"

MINGW_LIB = "/usr/x86_64-w64-mingw32/lib"
CURRENT_OS = platform.system()

# Source files (in AGENT_DIR)
SOURCES = [
    "main.cpp",
    "rdp_agent.cpp",
    "capture_wgc.cpp",
    "encoder_x264.cpp",
    "rtsp_client.cpp",
]

# RTSP_ reference source files (relative to PROJECT_ROOT)
RTSP_SOURCES = [
    "WinRT-API/capture_wgc.cpp",
    "WinRT-API/encoder_x264.cpp",
    "rtp/h264_rtp_packetizer.cpp",
]

TARGET_FLAGS = ["--target=x86_64-w64-windows-gnu", "-fms-extensions",
                f"-L{MINGW_LIB}"]
CXX = ["clang++-19"] + TARGET_FLAGS
CC = ["clang-19"] + TARGET_FLAGS
AR = "llvm-ar-19"


def find_build_slug(last_dir: Path) -> str:
    """Auto-increment build slug based on existing dist files."""
    if not last_dir.exists():
        return "1.0.0"
    existing = list(last_dir.glob("agent_*.exe"))
    if not existing:
        return "1.0.0"
    slugs = []
    for f in existing:
        name = f.stem  # agent_1.0.0
        parts = name.split("_")
        if len(parts) >= 2:
            try:
                slugs.append(tuple(map(int, parts[-1].split("."))))
            except ValueError:
                continue
    if not slugs:
        return "1.0.0"
    slugs.sort()
    major, minor, patch = slugs[-1]
    patch += 1
    if patch > 99:
        patch = 0
        minor += 1
    if minor > 99:
        minor = 0
        major += 1
    return f"{major}.{minor}.{patch}"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def check_source_files() -> bool:
    """Verify all source files exist, warn about missing ones."""
    all_ok = True
    for src in SOURCES:
        src_path = AGENT_DIR / src
        if not src_path.exists():
            print(f"[!] WARNING: source not found: {src_path}")
            all_ok = False
    for src in RTSP_SOURCES:
        src_path = PROJECT_ROOT / src
        if not src_path.exists():
            print(f"[!] WARNING: RTSP source not found: {src_path}")
            all_ok = False
    return all_ok


def check_deps() -> bool:
    """Verify cross-compiled dependencies exist."""
    ok = True
    needed = [
        DEPS_LIB / "libx264.a",
        DEPS_INCLUDE / "x264.h",
        DEPS_INCLUDE / "winrt" / "winrt" / "Windows.Graphics.Capture.h",
    ]
    for f in needed:
        if not f.exists():
            print(f"[!] Dependency missing: {f}")
            print(f"    Run: bash agent/builder/build_deps.sh")
            ok = False
    return ok


def build_exe(build_slug: str, server_url: str) -> Path:
    output_exe = DIST_DIR / f"agent_{build_slug}.exe"
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    print(f"[+] Building {output_exe.name}")
    print(f"[i] Compiler: {' '.join(CXX)}  Platform: {CURRENT_OS}")
    print(f"[i] Server URL: {server_url}")

    cmd = CXX + [
        "-std=c++17",
        "-O2",
        "-D_WIN32_WINNT=0x0602",
        "-DPSAPI_VERSION=2",
        "-o",
        str(output_exe),
        f'-DSERVER_URL="{server_url}"',
        f'-DBUILD_SLUG="{build_slug}"',
        f'-I{PROJECT_ROOT}',
        f'-I{DEPS_INCLUDE}',
        f'-I{DEPS_INCLUDE / "winrt"}',
        "-include",
        str(DEPS_INCLUDE / "winrt" / "winrt" / "wgc_numerics_fixup.h"),
    ]

    # Add agent source files
    for src in SOURCES:
        src_path = AGENT_DIR / src
        if src_path.exists():
            cmd.append(str(src_path))
        else:
            print(f"[!] Skipping missing source: {src}")

    # Add RTSP_ reference source files
    for src in RTSP_SOURCES:
        src_path = PROJECT_ROOT / src
        if src_path.exists():
            cmd.append(str(src_path))
        else:
            print(f"[!] Skipping missing RTSP source: {src}")

    # libx264
    cmd.append(f'-L{DEPS_LIB}')
    cmd.append('-l:libx264.a')

    # MinGW libpthread (forced whole-archive for clang's libstdc++/libgcc_eh)
    cmd.append(f'-L{MINGW_LIB}')
    cmd.extend(["-Xlinker", "--whole-archive", "-l:libpthread.a", "-Xlinker", "--no-whole-archive"])

    # RPC (UuidCreate, UuidToStringA, RpcStringFreeA)
    cmd.append("-l:librpcrt4.a")

    # DirectX
    cmd.extend([
        "-l:libd3d11.a",
        "-l:libdxgi.a",
        "-l:libdxguid.a",
    ])

    # Standard Windows libs
    cmd.extend([
        "-l:libwinhttp.a",
        "-l:libws2_32.a",
        "-l:libadvapi32.a",
        "-l:libuser32.a",
        "-l:libsecur32.a",
        "-l:libcrypt32.a",
        "-l:libwtsapi32.a",
        "-l:libuserenv.a",
        "-l:libnetapi32.a",
        "-l:libiphlpapi.a",
        "-l:libsetupapi.a",
        "-l:libuuid.a",
        "-l:libversion.a",
        "-l:libpsapi.a",
        "-l:libgdi32.a",
        "-l:libole32.a",
        "-l:liboleaut32.a",
        "-l:libcomctl32.a",
        "-l:libruntimeobject.a",
        "-static",
    ])

    if output_exe.exists():
        try:
            output_exe.unlink()
        except PermissionError:
            print("[!] Cannot delete existing file, trying to build anyway")

    print(f"[+] Running: {' '.join(cmd)}")
    if CURRENT_OS == "Windows":
        subprocess.run(" ".join(cmd), shell=True, check=True, cwd=str(AGENT_DIR))
    else:
        subprocess.run(cmd, check=True, cwd=str(AGENT_DIR))

    return output_exe


def default_server_url() -> str:
    """Read SITE_DOMAIN from .env, fallback to https://localhost."""
    env_path = PROJECT_ROOT / ".env"
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if line.startswith("SITE_DOMAIN="):
                domain = line.split("=", 1)[1].strip().strip("\"'")
                if domain:
                    return f"https://{domain}"
    return "https://localhost"


def main():
    parser = argparse.ArgumentParser(description="Build RTSP_ Windows agent")
    parser.add_argument("--slug", type=str, default=None,
                        help="Build slug (e.g. 1.0.0). Auto-increments if omitted.")
    parser.add_argument("--server-url", type=str, default=None,
                        help="VPS server URL (default: from .env SITE_DOMAIN)")
    args = parser.parse_args()
    if args.server_url is None:
        args.server_url = default_server_url()

    # Check dependencies
    if not check_deps():
        print("[!] Dependencies missing. Run build_deps.sh first.")
        sys.exit(1)

    # Determine build slug
    if args.slug:
        build_slug = args.slug
    else:
        build_slug = find_build_slug(DIST_DIR)
    print(f"[i] Build slug: {build_slug}")

    # Check source files
    check_source_files()

    # Build
    exe_path = build_exe(build_slug, args.server_url)

    sha256 = sha256_file(exe_path)
    print(f"[i] SHA256: {sha256}")
    print(f"[+] Agent built: {exe_path.name}")
    print(f"[+] Path: {exe_path}")

    print(f"[i] Run 'python agent/builder/build_setup.py' to package installer")


if __name__ == "__main__":
    main()