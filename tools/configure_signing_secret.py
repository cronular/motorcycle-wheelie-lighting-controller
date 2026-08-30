"""Encrypt a firmware signing key into a GitHub Actions repository secret."""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import urllib.request
from pathlib import Path

from nacl.public import PublicKey, SealedBox


def github_token() -> str:
    result = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        text=True,
        capture_output=True,
        check=True,
    )
    credentials = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    token = credentials.get("password", "")
    if not token:
        raise RuntimeError("No GitHub HTTPS credential is available")
    return token


def github_request(url: str, token: str, method: str = "GET", payload: dict | None = None) -> dict:
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(request) as response:
        body = response.read()
        return json.loads(body) if body else {}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True, help="owner/name")
    parser.add_argument("--private-key", required=True)
    parser.add_argument("--secret-name", default="FIRMWARE_SIGNING_KEY_PEM")
    args = parser.parse_args()

    token = github_token()
    base_url = f"https://api.github.com/repos/{args.repository}/actions/secrets"
    public = github_request(f"{base_url}/public-key", token)
    sealed_box = SealedBox(PublicKey(base64.b64decode(public["key"])))
    encrypted = base64.b64encode(
        sealed_box.encrypt(Path(args.private_key).read_bytes())
    ).decode("ascii")
    github_request(
        f"{base_url}/{args.secret_name}",
        token,
        method="PUT",
        payload={"encrypted_value": encrypted, "key_id": public["key_id"]},
    )
    print(f"Configured GitHub Actions secret {args.secret_name} for {args.repository}")


if __name__ == "__main__":
    main()
