from __future__ import annotations

import argparse
import re
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

from tools import package_firmware


class FirmwarePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.private_key = self.root / "private.pem"
        self.public_key = self.root / "public.pem"
        key = ec.generate_private_key(ec.SECP256R1())
        self.private_key.write_bytes(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        self.public_key.write_bytes(
            key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        self.firmware = self.root / "firmware.bin"
        self.firmware.write_bytes(bytes(range(256)) * 32)
        self.package = self.root / "firmware.wctrl"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def package_args(self) -> argparse.Namespace:
        return argparse.Namespace(
            firmware=str(self.firmware),
            private_key=str(self.private_key),
            output=str(self.package),
            board="seeed_xiao_esp32s3",
            chip="esp32s3",
            version="v0.13.0",
            channel="testing",
            commit="0123456789ab",
            built="2026-08-30T12:00:00Z",
        )

    def verify_args(self) -> argparse.Namespace:
        return argparse.Namespace(package=str(self.package), public_key=str(self.public_key))

    def test_signed_package_round_trip(self) -> None:
        package_firmware.package_firmware(self.package_args())
        package_firmware.verify_package(self.verify_args())
        manifest, _, firmware = package_firmware.parse_package(self.package)
        fields = package_firmware.parse_manifest(manifest)
        self.assertEqual(fields["board"], "seeed_xiao_esp32s3")
        self.assertEqual(fields["version"], "v0.13.0")
        self.assertEqual(fields["channel"], "testing")
        self.assertEqual(len(firmware), 8192)
        self.assertEqual(firmware, self.firmware.read_bytes())

    def test_firmware_tampering_is_rejected(self) -> None:
        package_firmware.package_firmware(self.package_args())
        payload = bytearray(self.package.read_bytes())
        payload[-1] ^= 0x01
        self.package.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            package_firmware.verify_package(self.verify_args())

    def test_manifest_signature_tampering_is_rejected(self) -> None:
        package_firmware.package_firmware(self.package_args())
        payload = bytearray(self.package.read_bytes())
        payload[package_firmware.HEADER.size + 10] ^= 0x01
        self.package.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "signature"):
            package_firmware.verify_package(self.verify_args())

    def test_embedded_and_published_public_keys_match(self) -> None:
        repository = Path(__file__).resolve().parents[1]
        public_lines = [
            line
            for line in (repository / "security" / "firmware-signing-public.pem")
            .read_text(encoding="ascii")
            .splitlines()
            if not line.startswith("-----")
        ]
        header = (repository / "include" / "firmware_signing_key.h").read_text(
            encoding="utf-8"
        )
        for line in public_lines:
            self.assertIn(line, header)

    def test_embedded_version_is_channel_agnostic_semver(self) -> None:
        repository = Path(__file__).resolve().parents[1]
        source = (repository / "src" / "main.cpp").read_text(encoding="utf-8")
        match = re.search(r'FIRMWARE_VERSION = "([^"]+)"', source)
        self.assertIsNotNone(match)
        self.assertRegex(match.group(1), r"^v\d+\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
