import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "include" / "firmware_runtime.h"
DASHBOARD = ROOT / "src" / "modules" / "dashboard_module.cpp"
NETWORK = ROOT / "src" / "modules" / "network_module.cpp"
OPERATING_MODE = ROOT / "src" / "modules" / "operating_mode_module.cpp"
STORAGE = ROOT / "src" / "modules" / "storage_module.cpp"


class CaptureModeTests(unittest.TestCase):
    def test_release_version_is_0151(self) -> None:
        self.assertIn('FIRMWARE_VERSION = "v0.15.1"', RUNTIME.read_text())

    def test_capture_is_a_distinct_operating_mode(self) -> None:
        runtime = RUNTIME.read_text()
        enum = re.search(r"enum class OperatingMode\s*\{([^}]+)\}", runtime, re.DOTALL)
        self.assertIsNotNone(enum)
        self.assertIn("CAPTURE", enum.group(1))

    def test_capture_page_and_route_are_registered(self) -> None:
        dashboard = DASHBOARD.read_text(encoding="utf-8")
        network = NETWORK.read_text(encoding="utf-8")
        self.assertIn("const char CAPTURE_HTML[] PROGMEM", dashboard)
        self.assertIn('href="/api/model/events.csv"', dashboard)
        self.assertIn('server.on("/capture", HTTP_GET, handleCapturePage)', network)

    def test_capture_mode_never_runs_controller_output_logic(self) -> None:
        source = OPERATING_MODE.read_text(encoding="utf-8")
        self.assertIn("if (operatingMode != OperatingMode::ARMED)", source)
        self.assertIn("setOutputTarget(0)", source)
        self.assertIn("case OperatingMode::CAPTURE: return \"CAPTURE\"", source)

    def test_capture_mode_collects_even_when_rider_lab_is_disabled(self) -> None:
        source = STORAGE.read_text(encoding="utf-8")
        self.assertIn(
            "if (!riderModelEnabled && operatingMode != OperatingMode::CAPTURE)",
            source,
        )

    def test_event_marker_requires_full_pre_event_buffer(self) -> None:
        source = DASHBOARD.read_text(encoding="utf-8")
        self.assertIn("modelPreEventCount < MODEL_PRE_EVENT_SAMPLES", source)
        self.assertIn("wait two seconds", source)

    def test_capture_exits_to_standby_on_physical_toggle(self) -> None:
        source = OPERATING_MODE.read_text(encoding="utf-8")
        capture_branch = re.search(
            r"else if \(operatingMode == OperatingMode::CAPTURE\)\s*\{(.*?)\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(capture_branch)
        self.assertIn("OperatingMode::STANDBY", capture_branch.group(1))


if __name__ == "__main__":
    unittest.main()
