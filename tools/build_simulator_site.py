"""Build the static GitHub Pages simulator, including embedded rider-phone pages."""

from __future__ import annotations

from argparse import ArgumentParser
from pathlib import Path
import os
import shutil

from run_simulator import PHONE_CAPTURE, PHONE_DASHBOARD, PHONE_SETTINGS, PROJECT_DIR, SIMULATOR_DIR


def build(output: Path) -> None:
    output = Path(os.path.abspath(output))
    allowed_root = Path(os.path.abspath(PROJECT_DIR / "dist"))
    if output == allowed_root or Path(os.path.commonpath((output, allowed_root))) != allowed_root:
        raise ValueError(f"Simulator output must be a child of {allowed_root}")
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(SIMULATOR_DIR, output)
    phone = output / "phone"
    settings = phone / "settings"
    capture = phone / "capture"
    settings.mkdir(parents=True)
    capture.mkdir(parents=True)
    (phone / "index.html").write_bytes(PHONE_DASHBOARD)
    (settings / "index.html").write_bytes(PHONE_SETTINGS)
    (capture / "index.html").write_bytes(PHONE_CAPTURE)


def verify(output: Path) -> None:
    lab = (output / "index.html").read_text(encoding="utf-8")
    dashboard = (output / "phone" / "index.html").read_text(encoding="utf-8")
    settings = (output / "phone" / "settings" / "index.html").read_text(
        encoding="utf-8"
    )
    capture = (output / "phone" / "capture" / "index.html").read_text(
        encoding="utf-8"
    )
    checks = {
        "relative phone iframe": 'src="phone/"' in lab,
        "phone API bridge": "wheelieSimulatorApi" in dashboard,
        "relative settings link": 'href="settings/"' in dashboard,
        "relative capture link": 'href="capture/"' in dashboard,
        "relative dashboard link": 'href="../"' in settings,
        "rider HUD": "Rider HUD" in dashboard,
        "settings page": "Controller Settings" in settings,
        "capture page": "Data Capture" in capture,
        "capture event button": 'id="eventBtn"' in capture,
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise RuntimeError("Static simulator verification failed: " + ", ".join(failed))


def main() -> None:
    parser = ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=PROJECT_DIR / "dist" / "simulator",
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = Path(os.path.abspath(args.output))
    build(output)
    if args.check:
        verify(output)
    print(f"Static simulator site built at {output}")


if __name__ == "__main__":
    main()
