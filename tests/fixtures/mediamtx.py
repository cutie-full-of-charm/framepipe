# tests/fixtures/mediamtx.py
import hashlib
import os
import platform
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path

import requests
from loguru import logger
from tqdm import tqdm

from .utils import MEDIAMTX_DIR, MEDIAMTX_EXECUTABLE

MEDIAMTX_VERSION_PINNED = "v1.19.1"
MEDIAMTX_SHA256_PINNED = {
    "mediamtx_v1.19.1_darwin_amd64.tar.gz": "f48b97796096eb70b175cd1881afa07c24d6238e92126ade95587e327ebaadb5",
    "mediamtx_v1.19.1_darwin_arm64.tar.gz": "217423874b56da675cc9627d110bb3a05fdd3b26e025980d801189e2ae3684d1",
    "mediamtx_v1.19.1_linux_amd64.tar.gz": "035ee04f91b1c7a0c02e13b2139ca2456e43b6bd6a80e3100e8c228556e07807",
    "mediamtx_v1.19.1_linux_arm64.tar.gz": "97a277cf24153e168008c18da53fe84e8d364456e2d7b457dc0457666c32867b",
    "mediamtx_v1.19.1_linux_armv6.tar.gz": "7c6a59ea3e2344f0dbae018cae594a52999917c3eb8169b9f7daa9eef9545bc0",
    "mediamtx_v1.19.1_linux_armv7.tar.gz": "052654f2268ad0604f2bb277e417cf3c122d7399f814e6d1ca2dbcf180ed7fe9",
    "mediamtx_v1.19.1_windows_amd64.zip": "ca3d89c370bf73dc33ef9ee87afea04834f21b129f499d4b645490fcb01cce22",
}


def verify_sha256(file_path, hash):
    if not file_path.is_file():
        return False

    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            sha256.update(chunk)
    return sha256.hexdigest().lower() == hash.lower()


def get_mediamtx_release_link():
    resp = requests.get(
        f"https://api.github.com/repos/bluenviron/mediamtx/releases/tags/{MEDIAMTX_VERSION_PINNED}"
    ).json()
    assets = resp["assets"]
    sys_name = platform.system()
    sys_arch = platform.uname().machine.lower()

    # cases:
    # darwin amd64
    # darwin arm64
    # linux amd64
    # linux arm64
    # linux arm6
    # linux amr7
    # windwos amd64
    asset = None
    if sys_name == "Linux":
        if sys_arch == "x86_64":
            asset = [a for a in assets if "linux_amd64.tar.gz" in a["name"]][0]
        if sys_arch == "aarch64":
            asset = [a for a in assets if "linux_arm64.tar.gz" in a["name"]][0]
        if sys_arch == "armv6":
            asset = [a for a in assets if "linux_armv6.tar.gz" in a["name"]][0]
        if sys_arch == "armv7":
            asset = [a for a in assets if "linux_armv7.tar.gz" in a["name"]][0]

    if sys_name == "Windows":
        asset = [a for a in assets if "windows_amd64.zip" in a["name"]][0]

    if sys_name == "Darwin":
        if sys_arch == "x86_64":
            asset = [a for a in assets if "darwin_amd64.tar.gz" in a["name"]][0]
        if sys_arch == "aarch64":
            asset = [a for a in assets if "darwin_arm64.tar.gz" in a["name"]][0]

    if asset is None:
        logger.error("Can't define release URL")
        logger.warning(f"Detected OS: {sys_name}, arch: {sys_arch}")
        return None

    logger.debug(f"Detected OS: {sys_name}, arch: {sys_arch}")
    browser_download_url = asset["browser_download_url"]
    logger.debug(f"Release url: {browser_download_url}")
    return browser_download_url


def download_asset(url, dest_path):
    logger.info(f"Downloading asset: {url}")
    dest_path.parent.mkdir(parents=True, exist_ok=True)

    tmp_download = dest_path.with_suffix(".tmp")
    try:
        resp = requests.get(url, stream=True, timeout=30)
        resp.raise_for_status()
        total_size = int(resp.headers.get("content-length", 0))

        with tqdm(total=total_size, unit="B", unit_scale=True) as pb:
            with open(tmp_download, "wb") as f:
                for data in resp.iter_content(65536):
                    f.write(data)
                    pb.update(len(data))

        os.replace(tmp_download, dest_path)
        logger.success(f"Downloaded and saved to cache: {dest_path.name}")
        return True
    except Exception as e:
        logger.error(f"Downloading error: {e}")
        if tmp_download.exists():
            tmp_download.unlink()
        return False


def unpack_and_configure_asset(
    archive_path: Path, dest_folder: Path, sys_name: str
):
    dest_folder.parent.mkdir(parents=True, exist_ok=True)
    logger.info("Unpacking and configuring...")

    with tempfile.TemporaryDirectory(dir=dest_folder.parent) as tmp_dir:
        tmp_path = Path(tmp_dir) / "unpacked_content"

        if sys_name == "Windows":
            with zipfile.ZipFile(archive_path, "r") as zip_ref:
                zip_ref.extractall(tmp_path)
        else:
            with tarfile.open(archive_path) as file:
                file.extractall(tmp_path)

        config_path = tmp_path / "mediamtx.yml"
        if config_path.is_file():
            logger.info("Modifying default config inside temp directory...")
            content = config_path.read_text(encoding="UTF-8")
            if "api: false" in content:
                content = content.replace("api: false", "api: true")

            config_path.write_text(content, encoding="UTF-8")
            logger.success("MediaMTX API configuration injected")

        if platform.system() == "Windows":
            if dest_folder.exists():
                shutil.rmtree(dest_folder)
            shutil.move(tmp_path, dest_folder)
        else:
            os.replace(tmp_path, dest_folder)

        logger.success(f"Atomic setup complete at: {dest_folder}")


def setup_mediamtx():
    dest_folder = MEDIAMTX_DIR
    sys_name = platform.system()

    link = get_mediamtx_release_link()
    if not link:
        logger.critical("Can't get release link!")
        return False

    asset_name = link.split("/")[-1]
    expected_hash = MEDIAMTX_SHA256_PINNED.get(asset_name)

    if not expected_hash:
        logger.critical(f"Can't find SHA256 hash for asset: {asset_name}")
        return False

    archive_cache_path = dest_folder.parent / "archive_cache" / asset_name

    if verify_sha256(archive_cache_path, expected_hash):
        logger.info(
            "[Cache Hit] Cached asset's SHA256 hash is correct. Skipping download."
        )
    else:
        logger.warning(
            "[Cache Miss] Archive missing or corrupted. Downloading..."
        )
        if not download_asset(link, archive_cache_path):
            return False

        if not verify_sha256(archive_cache_path, expected_hash):
            logger.critical("Downloaded asset SHA256 verification failed!")
            if archive_cache_path.exists():
                archive_cache_path.unlink()
            return False
        logger.success("Downloaded asset SHA256 verified successfully")

    if check_mediamtx():
        logger.info(
            "[Cache Hit] MediaMTX bin and config already installed and configured."
        )
        return True

    try:
        unpack_and_configure_asset(archive_cache_path, dest_folder, sys_name)
        return True
    except Exception as e:
        logger.error(f"Setup execution failed: {e}")
        return False


def check_mediamtx():
    executable_exists = Path(MEDIAMTX_DIR / MEDIAMTX_EXECUTABLE).is_file()
    config_exists = Path(MEDIAMTX_DIR / "mediamtx.yml").is_file()

    if executable_exists and config_exists:
        return True

    return False
