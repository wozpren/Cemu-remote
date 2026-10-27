# SPDX-License-Identifier: GPL-3.0-only
"""Summarize CPU-observed events from a user-recorded GamePad timeline.

This reads CSV only. It does not start Cemu, connect a client, or run a test.
GPU execution time and client decode/display time require separate measurements.
"""
import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


STAGES = {
    "source_to_gpu_ready_observed": ("source", "gpu_ready_observed"),
    "gpu_ready_to_encode_submit": ("gpu_ready_observed", "encode_submit"),
    "encode_submit_to_complete_observed": ("encode_submit", "encode_complete"),
    "encode_complete_to_bitstream_ready": ("encode_complete", "bitstream_ready"),
    "bitstream_to_first_packet_submitted": ("bitstream_ready", "first_packet_submitted"),
    "first_to_last_packet_submitted": ("first_packet_submitted", "last_packet_submitted"),
    "source_to_last_packet_submitted": ("source", "last_packet_submitted"),
}


def summarize(path):
    frames = defaultdict(dict)
    with path.open(newline="", encoding="utf-8-sig") as source:
        for line, row in enumerate(csv.DictReader(source), 2):
            try:
                key = (int(row["generation"]), int(row["source_sequence"]), int(row["source_ns"]))
                observed = int(row["monotonic_ns"])
                if min(*key, observed) < 0:
                    raise ValueError("negative timestamp or frame identifier")
                events = frames[key]
                event = row["event"]
                if event in events:
                    raise ValueError(f"duplicate event {event}")
                events["source"] = key[2]
                events[event] = observed
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(f"Invalid timeline row {line}: {error}") from error
    output = {"frames_observed": len(frames), "clock": "host monotonic nanoseconds, CPU observations",
              "percentile_method": "nearest rank", "unit": "milliseconds", "stages": {}}
    for name, (start, end) in STAGES.items():
        samples = []
        reversed_count = 0
        for events in frames.values():
            if start not in events or end not in events:
                continue
            delta = events[end] - events[start]
            if delta < 0:
                reversed_count += 1
            else:
                samples.append(delta / 1_000_000)
        samples.sort()
        record = {"samples": len(samples), "missing": len(frames) - len(samples) - reversed_count,
                  "out_of_order": reversed_count}
        if samples:
            for label, quantile in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
                record[label] = samples[math.ceil(quantile * len(samples)) - 1]
        output["stages"][name] = record
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.timeline), ensure_ascii=False, indent=2))
