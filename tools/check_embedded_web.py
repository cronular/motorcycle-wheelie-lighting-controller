"""Static consistency checks for the exact dashboard/settings HTML in firmware."""

from __future__ import annotations

import re

from device_preview import embedded_html


def check_page(name: str) -> None:
    page = embedded_html(name).decode("utf-8")
    element_ids = set(re.findall(r'id="([^"]+)"', page))
    references = set(re.findall(r"\$\(['\"]([^'\"]+)['\"]\)", page))
    missing = sorted(references - element_ids)
    if missing:
        raise RuntimeError(f"{name} references missing element IDs: {', '.join(missing)}")
    if page.count("<script>") != page.count("</script>"):
        raise RuntimeError(f"{name} has unbalanced script tags")


def main() -> None:
    check_page("DASHBOARD_HTML")
    check_page("SETTINGS_HTML")
    print("Embedded dashboard and settings references passed")


if __name__ == "__main__":
    main()
