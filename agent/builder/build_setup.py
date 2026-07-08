import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DIST_DIR = PROJECT_ROOT / "dist"
AGENTS_DIR = DIST_DIR / "agents"
BUILD_DIR = PROJECT_ROOT / "dist" / "setup_tmp"
OUTPUT = DIST_DIR / "agent_setup.exe"

INSTALL_CMD = PROJECT_ROOT / "agent" / "install.cmd"
UNINSTALL_CMD = PROJECT_ROOT / "agent" / "uninstall.cmd"

SFX_DIR = PROJECT_ROOT / "agent" / "sfx"
SFX_WIN = SFX_DIR / "7z.sfx"
SFX_DOWNLOAD_URL = "https://www.7-zip.org/a/7z2301.exe"
SFX_EXTRACT_NAME = "7z.sfx"


def get_latest_build_slug() -> str | None:
    agents = sorted(
        [f for f in os.listdir(AGENTS_DIR) if f.startswith("agent_") and not f.startswith("agent_setup_")],
        reverse=True,
    )
    if not agents:
        return None
    name = agents[0]
    return name.replace("agent_", "").replace(".exe", "")


def ensure_sfx():
    if SFX_WIN.exists():
        return
    SFX_DIR.mkdir(parents=True, exist_ok=True)
    installer_exe = SFX_DIR / "7z2301.exe"
    print(f"[+] Downloading 7-Zip for Windows (SFX module)...")
    urllib.request.urlretrieve(SFX_DOWNLOAD_URL, installer_exe)
    print(f"[+] Extracting {SFX_EXTRACT_NAME}...")
    subprocess.run(
        ["7z", "e", str(installer_exe), f"-o{SFX_DIR}", SFX_EXTRACT_NAME, "-y"],
        check=True, capture_output=True,
    )
    installer_exe.unlink()
    if SFX_WIN.exists():
        print(f"[+] {SFX_EXTRACT_NAME} ready ({SFX_WIN.stat().st_size} bytes)")
    else:
        print(f"[!] {SFX_EXTRACT_NAME} not found in 7z installer, aborting")
        sys.exit(1)


def build_setup(build_slug: str):
    agent_exe = AGENTS_DIR / f"agent_{build_slug}.exe"
    if not agent_exe.exists():
        agent_exe = AGENTS_DIR / "agent.exe"
    if not agent_exe.exists():
        print(f"[!] Agent exe not found in {AGENTS_DIR}")
        sys.exit(1)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BUILD_DIR_EXTRACT = BUILD_DIR / "extract"
    if BUILD_DIR_EXTRACT.exists():
        shutil.rmtree(BUILD_DIR_EXTRACT)
    BUILD_DIR_EXTRACT.mkdir(parents=True, exist_ok=True)

    print(f"[+] Building RTSP_ agent installer (v{build_slug})")
    print(f"[i] Agent: {agent_exe.name} ({agent_exe.stat().st_size // 1024} KB)")

    for src, dst_name in [
        (agent_exe, "agent.exe"),
        (INSTALL_CMD, "install.cmd"),
        (UNINSTALL_CMD, "uninstall.cmd"),
    ]:
        if not src.exists():
            print(f"[!] Missing: {src}")
            sys.exit(1)
        shutil.copy2(src, BUILD_DIR_EXTRACT / dst_name)
        print(f"[i] Packing: {dst_name}")

    archive_7z = BUILD_DIR / "agent_setup.7z"
    if archive_7z.exists():
        archive_7z.unlink()
    subprocess.run(
        ["7z", "a", "-mx=7", "-ms=on", str(archive_7z), "."],
        cwd=str(BUILD_DIR_EXTRACT), check=True, capture_output=True,
    )
    print(f"[+] Archive created: {archive_7z} ({archive_7z.stat().st_size // 1024} KB)")

    config = b";!@Install@!UTF-8!\r\nTitle=\"RTSP Agent Setup\"\r\nBeginPrompt=\"Install RTSP Desktop Agent?\"\r\nRunProgram=\"install.cmd\"\r\nGUIMode=\"2\"\r\n;!@InstallEnd@!\r\n"

    with open(OUTPUT, "wb") as f:
        f.write(SFX_WIN.read_bytes())
        f.write(config)
        f.write(archive_7z.read_bytes())

    os.chmod(OUTPUT, 0o755)

    if OUTPUT.exists():
        size_kb = OUTPUT.stat().st_size // 1024
        print(f"[+] Setup created: {OUTPUT} ({size_kb} KB)")
    else:
        print(f"[!] Output not found: {OUTPUT}")
        sys.exit(1)


def main():
    build_slug = get_latest_build_slug()
    if not build_slug:
        print("[!] No agent_rtsp_*.exe found in dist/agents/")
        print("    Run build_agents.py first")
        sys.exit(1)
    ensure_sfx()
    build_setup(build_slug)


if __name__ == "__main__":
    main()
