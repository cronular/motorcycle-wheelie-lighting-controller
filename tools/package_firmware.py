"""Create and verify signed Wheelie Controller OTA packages.

The private ECDSA P-256 key is used only by release automation. Devices embed
the matching public key and verify the signed manifest plus streamed firmware
SHA-256 before activating an OTA partition.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import re
import struct
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


MAGIC = b"WCTRL1\r\n"
HEADER = struct.Struct("<8sHHI")
FIELD_PATTERN = re.compile(r"^[A-Za-z0-9._:+-]+$")
REQUIRED_FIELDS = (
    "format",
    "board",
    "chip",
    "version",
    "channel",
    "commit",
    "built",
    "size",
    "sha256",
)


def safe_field(name: str, value: str) -> str:
    if not value or not FIELD_PATTERN.fullmatch(value):
        raise ValueError(f"Invalid {name}: use letters, digits, '.', '_', ':', '+', or '-'")
    return value


def create_manifest(args: argparse.Namespace, firmware: bytes) -> bytes:
    fields = {
        "format": "1",
        "board": safe_field("board", args.board),
        "chip": safe_field("chip", args.chip),
        "version": safe_field("version", args.version),
        "channel": safe_field("channel", args.channel),
        "commit": safe_field("commit", args.commit),
        "built": safe_field("built", args.built),
        "size": str(len(firmware)),
        "sha256": hashlib.sha256(firmware).hexdigest(),
    }
    return "".join(f"{key}={fields[key]}\n" for key in REQUIRED_FIELDS).encode("ascii")


def parse_package(path: Path) -> tuple[bytes, bytes, bytes]:
    package = path.read_bytes()
    if len(package) < HEADER.size:
        raise ValueError("Package is shorter than its header")
    magic, manifest_size, signature_size, firmware_size = HEADER.unpack_from(package)
    if magic != MAGIC:
        raise ValueError("Not a Wheelie Controller OTA package")
    expected_size = HEADER.size + manifest_size + signature_size + firmware_size
    if expected_size != len(package):
        raise ValueError("Package length does not match its header")
    manifest_start = HEADER.size
    signature_start = manifest_start + manifest_size
    firmware_start = signature_start + signature_size
    return (
        package[manifest_start:signature_start],
        package[signature_start:firmware_start],
        package[firmware_start:],
    )


def parse_manifest(manifest: bytes) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in manifest.decode("ascii").splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in fields:
            raise ValueError("Malformed manifest")
        fields[key] = value
    if tuple(fields) != REQUIRED_FIELDS:
        raise ValueError("Manifest fields or ordering are invalid")
    return fields


def generate_key(args: argparse.Namespace) -> None:
    private_path = Path(args.private_key)
    public_path = Path(args.public_key)
    if private_path.exists() or public_path.exists():
        raise FileExistsError("Refusing to overwrite an existing signing key")
    private_key = ec.generate_private_key(ec.SECP256R1())
    private_path.write_bytes(
        private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    public_path.write_bytes(
        private_key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )
    print(f"Generated public key: {public_path}")
    print("Private key generated separately; store it only as FIRMWARE_SIGNING_KEY_PEM.")


def package_firmware(args: argparse.Namespace) -> None:
    firmware_path = Path(args.firmware)
    output_path = Path(args.output)
    firmware = firmware_path.read_bytes()
    manifest = create_manifest(args, firmware)
    private_key = serialization.load_pem_private_key(
        Path(args.private_key).read_bytes(), password=None
    )
    if not isinstance(private_key, ec.EllipticCurvePrivateKey) or not isinstance(
        private_key.curve, ec.SECP256R1
    ):
        raise ValueError("Signing key must be ECDSA P-256")
    signature = private_key.sign(manifest, ec.ECDSA(hashes.SHA256()))
    header = HEADER.pack(MAGIC, len(manifest), len(signature), len(firmware))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(header + manifest + signature + firmware)
    output_path.with_suffix(".manifest").write_bytes(manifest)
    output_path.with_suffix(".sig").write_bytes(signature)
    print(f"Created signed package: {output_path}")


def verify_package(args: argparse.Namespace) -> None:
    manifest, signature, firmware = parse_package(Path(args.package))
    public_key = serialization.load_pem_public_key(Path(args.public_key).read_bytes())
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(
        public_key.curve, ec.SECP256R1
    ):
        raise ValueError("Verification key must be ECDSA P-256")
    try:
        public_key.verify(signature, manifest, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as error:
        raise ValueError("Manifest signature is invalid") from error
    fields = parse_manifest(manifest)
    if fields["format"] != "1":
        raise ValueError("Unsupported manifest format")
    if int(fields["size"]) != len(firmware):
        raise ValueError("Firmware size does not match the signed manifest")
    if not hmac.compare_digest(fields["sha256"], hashlib.sha256(firmware).hexdigest()):
        raise ValueError("Firmware SHA-256 does not match the signed manifest")
    print(
        f"Verified {fields['board']} {fields['version']} ({fields['channel']}) "
        f"commit {fields['commit']}"
    )


def sign_file(args: argparse.Namespace) -> None:
    source = Path(args.file)
    private_key = serialization.load_pem_private_key(
        Path(args.private_key).read_bytes(), password=None
    )
    if not isinstance(private_key, ec.EllipticCurvePrivateKey) or not isinstance(
        private_key.curve, ec.SECP256R1
    ):
        raise ValueError("Signing key must be ECDSA P-256")
    signature_path = Path(args.output) if args.output else source.with_suffix(source.suffix + ".sig")
    signature_path.write_bytes(private_key.sign(source.read_bytes(), ec.ECDSA(hashes.SHA256())))
    print(f"Created detached signature: {signature_path}")


def verify_file(args: argparse.Namespace) -> None:
    public_key = serialization.load_pem_public_key(Path(args.public_key).read_bytes())
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(
        public_key.curve, ec.SECP256R1
    ):
        raise ValueError("Verification key must be ECDSA P-256")
    try:
        public_key.verify(
            Path(args.signature).read_bytes(),
            Path(args.file).read_bytes(),
            ec.ECDSA(hashes.SHA256()),
        )
    except InvalidSignature as error:
        raise ValueError("Detached signature is invalid") from error
    print(f"Verified detached signature: {args.file}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    keygen = commands.add_parser("keygen", help="generate a P-256 signing key pair")
    keygen.add_argument("--private-key", required=True)
    keygen.add_argument("--public-key", required=True)
    keygen.set_defaults(handler=generate_key)

    package = commands.add_parser("package", help="sign and package firmware.bin")
    package.add_argument("--firmware", required=True)
    package.add_argument("--private-key", required=True)
    package.add_argument("--output", required=True)
    package.add_argument("--board", default="seeed_xiao_esp32s3")
    package.add_argument("--chip", default="esp32s3")
    package.add_argument("--version", required=True)
    package.add_argument("--channel", choices=("stable", "testing"), required=True)
    package.add_argument("--commit", required=True)
    package.add_argument("--built", required=True)
    package.set_defaults(handler=package_firmware)

    verify = commands.add_parser("verify", help="verify a signed OTA package")
    verify.add_argument("--package", required=True)
    verify.add_argument("--public-key", required=True)
    verify.set_defaults(handler=verify_package)

    detached_sign = commands.add_parser("sign-file", help="create a detached P-256 signature")
    detached_sign.add_argument("--file", required=True)
    detached_sign.add_argument("--private-key", required=True)
    detached_sign.add_argument("--output")
    detached_sign.set_defaults(handler=sign_file)

    detached_verify = commands.add_parser("verify-file", help="verify a detached P-256 signature")
    detached_verify.add_argument("--file", required=True)
    detached_verify.add_argument("--signature", required=True)
    detached_verify.add_argument("--public-key", required=True)
    detached_verify.set_defaults(handler=verify_file)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
