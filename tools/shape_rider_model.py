#!/usr/bin/env python3
"""Fit and validate a transparent shadow event model from controller exports.

The tool intentionally uses only the Python standard library. It groups
validation by complete ride session so adjacent events from one ride cannot
leak into both training and validation folds.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import sys
from dataclasses import dataclass


FEATURES = (
    "duration_ms",
    "samples",
    "above_trigger_samples",
    "pitch_rise",
    "peak_pitch_rate",
    "rms_pitch_rate",
    "integrated_positive_rate",
    "peak_g",
    "rms_g",
    "peak_abs_roll",
    "frozen_fraction",
)
POSITIVE_LABELS = {"correct", "missed"}
NEGATIVE_LABELS = {"false"}


@dataclass
class Example:
    group: str
    event_id: str
    label: int
    values: list[float]


def load_examples(paths: list[pathlib.Path]) -> tuple[list[Example], int]:
    examples: list[Example] = []
    ignored = 0
    for path in paths:
        with path.open(newline="", encoding="utf-8-sig") as handle:
            reader = csv.DictReader(handle)
            missing = [name for name in FEATURES if name not in (reader.fieldnames or [])]
            if missing:
                raise ValueError(f"{path}: missing columns: {', '.join(missing)}")
            for row_number, row in enumerate(reader, 2):
                label_name = (row.get("label") or "").strip().lower()
                if label_name not in POSITIVE_LABELS | NEGATIVE_LABELS:
                    ignored += 1
                    continue
                try:
                    values = [float(row[name]) for name in FEATURES]
                except (TypeError, ValueError) as error:
                    raise ValueError(f"{path}:{row_number}: invalid numeric feature") from error
                if not all(math.isfinite(value) for value in values):
                    raise ValueError(f"{path}:{row_number}: non-finite numeric feature")
                ride = (row.get("ride_session_id") or "0").strip()
                # Session zero means logging was off. Keep different exports
                # separate so they do not collapse into one validation group.
                group = f"{path.name}:unlogged" if ride in {"", "0"} else f"ride:{ride}"
                examples.append(Example(
                    group=group,
                    event_id=(row.get("event_id") or str(row_number)).strip(),
                    label=1 if label_name in POSITIVE_LABELS else 0,
                    values=values,
                ))
    return examples, ignored


def feature_scaler(examples: list[Example]) -> tuple[list[float], list[float]]:
    count = len(examples)
    means = [sum(example.values[i] for example in examples) / count for i in range(len(FEATURES))]
    scales = []
    for i, mean in enumerate(means):
        variance = sum((example.values[i] - mean) ** 2 for example in examples) / max(1, count - 1)
        scales.append(max(math.sqrt(variance), 1e-6))
    return means, scales


def sigmoid(value: float) -> float:
    value = max(-30.0, min(30.0, value))
    return 1.0 / (1.0 + math.exp(-value))


def train_logistic(
    examples: list[Example], iterations: int = 2500, learning_rate: float = 0.08
) -> tuple[list[float], list[float], list[float]]:
    means, scales = feature_scaler(examples)
    weights = [0.0] * (len(FEATURES) + 1)
    positives = sum(example.label for example in examples)
    negatives = len(examples) - positives
    positive_weight = len(examples) / max(1, 2 * positives)
    negative_weight = len(examples) / max(1, 2 * negatives)

    for iteration in range(iterations):
        gradient = [0.0] * len(weights)
        for example in examples:
            normalized = [
                (example.values[i] - means[i]) / scales[i]
                for i in range(len(FEATURES))
            ]
            probability = sigmoid(weights[0] + sum(
                weights[i + 1] * normalized[i] for i in range(len(FEATURES))))
            sample_weight = positive_weight if example.label else negative_weight
            error = (probability - example.label) * sample_weight
            gradient[0] += error
            for i, value in enumerate(normalized):
                gradient[i + 1] += error * value
        rate = learning_rate / (1.0 + iteration / 800.0)
        for i in range(len(weights)):
            regularization = 0.001 * weights[i] if i else 0.0
            weights[i] -= rate * (gradient[i] / len(examples) + regularization)
    return weights, means, scales


def predict(example: Example, model: tuple[list[float], list[float], list[float]]) -> float:
    weights, means, scales = model
    score = weights[0]
    for i, value in enumerate(example.values):
        score += weights[i + 1] * ((value - means[i]) / scales[i])
    return sigmoid(score)


def metrics(examples: list[Example], model) -> dict[str, float | int]:
    tp = tn = fp = fn = 0
    for example in examples:
        prediction = predict(example, model) >= 0.5
        if prediction and example.label:
            tp += 1
        elif prediction:
            fp += 1
        elif example.label:
            fn += 1
        else:
            tn += 1
    precision = tp / max(1, tp + fp)
    recall = tp / max(1, tp + fn)
    return {
        "events": len(examples),
        "true_positive": tp,
        "true_negative": tn,
        "false_positive": fp,
        "false_negative": fn,
        "precision": precision,
        "recall": recall,
        "f1": 2 * precision * recall / max(1e-9, precision + recall),
        "accuracy": (tp + tn) / max(1, len(examples)),
    }


def leave_one_ride_out(examples: list[Example]) -> dict[str, float | int] | None:
    groups = sorted({example.group for example in examples})
    predictions: list[tuple[int, bool]] = []
    for group in groups:
        train = [example for example in examples if example.group != group]
        test = [example for example in examples if example.group == group]
        if not train or len({example.label for example in train}) < 2:
            continue
        model = train_logistic(train)
        predictions.extend((example.label, predict(example, model) >= 0.5) for example in test)
    if not predictions:
        return None
    tp = sum(label == 1 and predicted for label, predicted in predictions)
    tn = sum(label == 0 and not predicted for label, predicted in predictions)
    fp = sum(label == 0 and predicted for label, predicted in predictions)
    fn = sum(label == 1 and not predicted for label, predicted in predictions)
    precision = tp / max(1, tp + fp)
    recall = tp / max(1, tp + fn)
    return {
        "events": len(predictions), "true_positive": tp, "true_negative": tn,
        "false_positive": fp, "false_negative": fn,
        "precision": precision, "recall": recall,
        "f1": 2 * precision * recall / max(1e-9, precision + recall),
        "accuracy": (tp + tn) / len(predictions),
    }


def export_model(model, examples: list[Example], ignored: int) -> dict:
    weights, means, scales = model
    return {
        "format": "wheelie-shadow-logistic",
        "version": 1,
        "authority": "advisory-only",
        "threshold": 0.5,
        "training": {
            "labeled_events": len(examples),
            "ignored_unlabeled_events": ignored,
            "ride_groups": len({example.group for example in examples}),
            "positive_events": sum(example.label for example in examples),
            "negative_events": sum(1 - example.label for example in examples),
        },
        "features": [
            {"name": name, "mean": means[i], "scale": scales[i], "weight": weights[i + 1]}
            for i, name in enumerate(FEATURES)
        ],
        "intercept": weights[0],
        "training_metrics": metrics(examples, model),
        "leave_one_ride_out_metrics": leave_one_ride_out(examples),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+", type=pathlib.Path, help="downloaded model event CSV files")
    parser.add_argument("--output", "-o", type=pathlib.Path, help="write the versioned model JSON")
    args = parser.parse_args(argv)
    try:
        examples, ignored = load_examples(args.csv)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if len(examples) < 4 or len({example.label for example in examples}) < 2:
        parser.error("at least four labeled events containing both intentional and false examples are required")

    result = export_model(train_logistic(examples), examples, ignored)
    rendered = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        print(rendered)

    validation = result["leave_one_ride_out_metrics"]
    if validation is None:
        print("WARNING: collect labeled events from at least two separable rides for leakage-safe validation.", file=sys.stderr)
    elif validation["events"] < 20:
        print("WARNING: validation is preliminary; collect at least 20 labeled events across several rides.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
