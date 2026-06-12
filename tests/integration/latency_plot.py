#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Bar chart of a before/after latency A/B from measure_latency.py JSON dumps,
with mean +/- 2-sigma error bars (p95 and max annotated, since send-path fixes
act on the tail).

    uv run latency_plot.py --stream rtsp --out out.png \
        "baseline=before.json" "fixed=after.json"

Each positional arg is `label=path` to a measure_latency.py --json-out file.
--metric cadence (default, inter-arrival ms; pair with --stall) or latency (OSD).
"""

import argparse
import json
import sys

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load_samples(path: str, stream: str, metric: str) -> tuple[np.ndarray, dict]:
    data = json.load(open(path))
    section = data.get("cadence" if metric == "cadence" else "latency_ms", {})
    entry = section.get(stream)
    if not entry or "samples_ms" in entry and not entry["samples_ms"]:
        raise SystemExit(f"{path}: no {metric} samples for stream '{stream}'")
    if "error" in entry:
        raise SystemExit(f"{path}: {stream} {metric} errored: {entry['error']}")
    return np.asarray(entry["samples_ms"], float), data


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot before/after latency A/B with 2-sigma error bars.")
    ap.add_argument("runs", nargs="+", metavar="label=path", help="labelled JSON runs to compare")
    ap.add_argument("--stream", default="rtsp", help="which stream to plot")
    ap.add_argument("--metric", choices=["cadence", "latency"], default="cadence")
    ap.add_argument("--out", required=True, help="output PNG path")
    ap.add_argument("--title", default=None)
    ap.add_argument("--ylabel", default=None)
    args = ap.parse_args()

    labels, means, twosigma, p95s, maxes, stall = [], [], [], [], [], None
    for run in args.runs:
        if "=" not in run:
            raise SystemExit(f"expected label=path, got '{run}'")
        label, path = run.split("=", 1)
        s, data = load_samples(path, args.stream, args.metric)
        labels.append(label)
        means.append(s.mean())
        twosigma.append(2 * s.std())
        p95s.append(np.percentile(s, 95))
        maxes.append(s.max())
        stall = (data.get("args") or {}).get("stall") or stall

    x = np.arange(len(labels))
    colors = ["#c0392b", "#27ae60", "#2980b9", "#8e44ad"][: len(labels)]
    fig, ax = plt.subplots(figsize=(1.7 * len(labels) + 1.5, 4.2))
    ax.bar(x, means, yerr=twosigma, capsize=8, color=colors, width=0.55,
           error_kw={"elinewidth": 1.6, "ecolor": "#222"})
    ymax = max(m + t for m, t in zip(means, twosigma))
    ax.set_ylim(0, ymax * 1.28)
    for i, (m, t2, p, mx) in enumerate(zip(means, twosigma, p95s, maxes)):
        y = min(m + t2 + ymax * 0.02, ymax * 1.05)
        ax.annotate(f"mean {m:.0f}\np95 {p:.0f}\nmax {mx:.0f}",
                    (i, y), ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    metric_name = "frame inter-arrival" if args.metric == "cadence" else "OSD inject-to-receive"
    ax.set_ylabel(args.ylabel or f"{metric_name} (ms)")
    title = args.title or f"{args.stream.upper()} {metric_name}, mean ±2σ"
    if stall and not args.title:
        title += f"\n(under a slow {stall} competitor)"
    ax.set_title(title, fontsize=11)
    ax.yaxis.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"wrote {args.out}")
    for label, m, t2, p, mx in zip(labels, means, twosigma, p95s, maxes):
        print(f"  {label:12s} mean={m:6.1f}  2σ={t2:6.1f}  p95={p:6.1f}  max={mx:6.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
