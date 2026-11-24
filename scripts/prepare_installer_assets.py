#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the pinned QEMU firmware and Microsoft-attested virtio-win ARM64 NetKVM package."""
import argparse
import bz2
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path(__file__).with_name("installer_assets.json")
QEMU = "https://raw.githubusercontent.com/qemu/qemu/698104725efad4b29079d857dfdebbd804e34c99/pc-bios/"
VIRTIO = "https://github.com/microsoft/openvmm-deps/releases/download/0.3.0-185/"
VIRTIO_ASSET = "openvmm-test-virtio-win.0.3.0-185.tar.gz"
VIRTIO_SHA256 = "b7ae2e60b8ae4b1a749bf8d03d555a4cf47684d724acab344318d148f6894e6e"
VIRTIO_LICENSES = ("https://raw.githubusercontent.com/virtio-win/kvm-guest-drivers-windows/"
                   "bd965ef1ad9bb380496d7386eb4afb02ddee6514/NetKVM/")
CATALOG_SIGNER = "b11d258150b8b6ba35b6da64f5082c515202fd1e1cebbaea5ff172073bc85699"

FIRMWARE_INPUT = {
    "edk2-aarch64-code.fd": "c023444108b7a132fdebf70c4765cd2dd9af2a9ff7d001a743aaabe87c20a458",
    "edk2-arm-vars.fd": "0011d83b5e67c691a58e3ae25a2e7fdf1d245a73d1b2b8bccb24af1a8c2e2523",
}


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(cache, url, expected):
    archive = cache / expected
    if archive.is_file() and file_hash(archive) == expected:
        return archive
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        download = Path(temporary) / "download"
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "2",
                        "--max-time", "60", "--proto", "=https", "--proto-redir", "=https",
                        "--output", download, url], check=True)
        if file_hash(download) != expected:
            raise ValueError(f"Checksum mismatch for {url}")
        download.replace(archive)
    return archive


def validate_netkvm(directory):
    text = (directory / "netkvm.inf").read_text(encoding="ascii")
    if "NTARM64" not in text.upper() or "netkvmp.exe" not in text.lower():
        raise ValueError("NetKVM INF lacks its Windows ARM64 target or protocol helper")
    for name in ("netkvm.sys", "netkvmp.exe"):
        data = (directory / name).read_bytes()
        offset, = struct.unpack_from("<I", data, 60)
        if data[:2] != b"MZ" or data[offset:offset + 6] != b"PE\0\0\x64\xaa":
            raise ValueError(f"{name} is not an ARM64 PE binary")
    with tempfile.TemporaryDirectory() as temporary:
        signer = Path(temporary) / "signer.pem"
        subprocess.run(["openssl", "smime", "-verify", "-binary", "-inform", "DER", "-noverify",
                        "-in", directory / "netkvm.cat", "-out", "/dev/null", "-signer", signer], check=True)
        leaf = subprocess.check_output(["openssl", "x509", "-in", signer, "-noout", "-fingerprint", "-sha256"],
                                       text=True)
        fingerprint = leaf.strip().split("=")[-1].replace(":", "").lower()
        if fingerprint != CATALOG_SIGNER:
            raise ValueError("NetKVM catalog was not signed by the pinned Microsoft publisher certificate")


def prepare(cache, output):
    manifest = json.loads(MANIFEST.read_text())
    cache.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        raise ValueError("InstallerAssets output must be a directory")
    if output.exists():
        for item in output.rglob("*"):
            name = item.relative_to(output).as_posix()
            if item.is_symlink() or (name not in manifest and name != "NetKVM"):
                raise ValueError(f"Unexpected installer output entry: {name}")
    if all((output / name).is_file() and file_hash(output / name) == expected
           for name, expected in manifest.items()):
        print("Using verified installer resources")
        return
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / "InstallerAssets"
        drivers = stage / "NetKVM"
        drivers.mkdir(parents=True)
        for name, compressed in FIRMWARE_INPUT.items():
            source = fetch(cache, QEMU + name + ".bz2", compressed)
            with bz2.open(source, "rb") as data, (stage / name).open("wb") as image:
                shutil.copyfileobj(data, image)
        for name, url in {
            "EDK2-Licenses.txt": QEMU + "edk2-licenses.txt",
            "NetKVM-LICENSE.txt": VIRTIO_LICENSES + "LICENSE",
            "NetKVM-ProtocolService-LICENSE.txt": VIRTIO_LICENSES + "ProtocolService/LICENSE",
        }.items():
            shutil.copyfile(fetch(cache, url, manifest[name]), stage / name)

        archive = fetch(cache, VIRTIO + VIRTIO_ASSET, VIRTIO_SHA256)
        with tarfile.open(archive, "r:gz") as package:
            for path in manifest:
                if not path.startswith("NetKVM/"):
                    continue
                name = Path(path).name
                member = package.getmember("./NetKVM/w11/ARM64/" + name)
                if not member.isfile() or not 0 < member.size <= 8 << 20:
                    raise ValueError(f"Invalid archive entry: {member.name}")
                with package.extractfile(member) as data, (drivers / name).open("wb") as file:
                    shutil.copyfileobj(data, file)
        for name, expected in manifest.items():
            if file_hash(stage / name) != expected:
                raise ValueError(f"Installer resource failed verification: {name}")
        validate_netkvm(drivers)
        if output.exists():
            (output / "NetKVM").mkdir(exist_ok=True)
            for name in manifest:
                (stage / name).replace(output / name)
        else:
            stage.rename(output)
    print("Verified and staged QEMU firmware, NetKVM, and their licenses")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=ROOT / "ios/deps/installer-cache")
    parser.add_argument("--output", type=Path, default=ROOT / "ios/Sources/Resources/InstallerAssets")
    args = parser.parse_args()
    prepare(args.cache, args.output)


if __name__ == "__main__":
    main()
