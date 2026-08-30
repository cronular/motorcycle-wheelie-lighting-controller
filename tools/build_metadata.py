"""Inject reproducible build diagnostics into the ESP32 firmware."""

from __future__ import annotations

import datetime as dt
import os
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def git_value(*args: str, fallback: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], cwd=env.subst("$PROJECT_DIR"), text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return fallback


def safe_literal(value: str) -> str:
    return value.replace("\\", "_").replace('"', "_").replace("\n", "_")


commit = os.environ.get("BUILD_COMMIT") or git_value(
    "rev-parse", "--short=12", "HEAD", fallback="unknown"
)
build_date = os.environ.get("BUILD_DATE") or dt.datetime.now(
    dt.timezone.utc
).replace(microsecond=0).isoformat().replace("+00:00", "Z")
channel = os.environ.get("RELEASE_CHANNEL", "testing").lower()
if channel not in {"stable", "testing"}:
    channel = "testing"

generated_directory = Path(env.subst("$BUILD_DIR")) / "generated"
generated_directory.mkdir(parents=True, exist_ok=True)
header = generated_directory / "build_metadata.generated.h"
contents = (
    "#pragma once\n"
    f'#define BUILD_COMMIT "{safe_literal(commit)}"\n'
    f'#define BUILD_DATE "{safe_literal(build_date)}"\n'
    f'#define RELEASE_CHANNEL "{safe_literal(channel)}"\n'
)
if not header.exists() or header.read_text(encoding="utf-8") != contents:
    header.write_text(contents, encoding="utf-8")
env.Append(CPPPATH=[str(generated_directory)])
