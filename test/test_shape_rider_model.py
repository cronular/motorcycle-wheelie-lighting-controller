import csv
import importlib.util
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "shape_rider_model", ROOT / "tools" / "shape_rider_model.py"
)
shape = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = shape
SPEC.loader.exec_module(shape)


class ShapeRiderModelTests(unittest.TestCase):
    def test_load_train_and_group_validation(self):
        rows = []
        for ride in (1, 2, 3):
            rows.extend([
                {
                    "ride_session_id": ride, "event_id": ride * 10 + 1,
                    "label": "correct", "duration_ms": 1200, "samples": 160,
                    "above_trigger_samples": 80, "pitch_rise": 28,
                    "peak_pitch_rate": 66, "rms_pitch_rate": 24,
                    "integrated_positive_rate": 32, "peak_g": 0.4,
                    "rms_g": 0.18, "peak_abs_roll": 8, "frozen_fraction": 0.2,
                },
                {
                    "ride_session_id": ride, "event_id": ride * 10 + 2,
                    "label": "false", "duration_ms": 500, "samples": 60,
                    "above_trigger_samples": 4, "pitch_rise": 5,
                    "peak_pitch_rate": 14, "rms_pitch_rate": 7,
                    "integrated_positive_rate": 3, "peak_g": 1.3,
                    "rms_g": 0.7, "peak_abs_roll": 18, "frozen_fraction": 0.8,
                },
            ])

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.csv"
            with path.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            examples, ignored = shape.load_examples([path])

        self.assertEqual(6, len(examples))
        self.assertEqual(0, ignored)
        result = shape.export_model(shape.train_logistic(examples), examples, ignored)
        self.assertEqual("advisory-only", result["authority"])
        self.assertEqual(3, result["training"]["ride_groups"])
        self.assertIsNotNone(result["leave_one_ride_out_metrics"])
        self.assertGreaterEqual(result["training_metrics"]["accuracy"], 0.8)


if __name__ == "__main__":
    unittest.main()
