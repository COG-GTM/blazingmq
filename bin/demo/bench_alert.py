#!/usr/bin/env python3
# Copyright 2026 Bloomberg Finance L.P.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Run the bmqt::Uri benchmark, compare it to the committed baseline and post
an on-call alert to Slack when parsing has regressed.

Usage:
    bin/demo/bench_alert.py [--build-dir build/blazingmq] [--runs 5]
                            [--threshold 1.5] [--dry-run] [--no-alert]

Exit code: 0 = within threshold, 2 = regression detected, 1 = error.

Slack: posts with the first of $SLACK_BOT_TOKEN, $SLACK_ONCALL_BOT_TOKEN,
$COG_GTM_DEMO_SLACK_BOT_TOKEN to $SLACK_ALERT_CHANNEL (default C0C6T4QNT2R,
#blazingmq-sre-alerts).

NOTE: this is demo tooling for the COG-GTM fork; the URI-parser regression it
detects is deliberately simulated.
"""

import argparse
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().parent / "baseline.json"
LINE_RE = re.compile(
    r"^(bmqt::UriParser::parse|bmqt::Uri::Uri)\s+threads=(\d+)\S*\s+([\d.]+)\s+(ns|us|ms|s)\b"
)
TO_MS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}


def git(*args):
    return subprocess.run(
        ["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=False
    ).stdout.strip()


def run_benchmark(binary, runs):
    samples = {}
    for i in range(runs):
        out = subprocess.run(
            [str(binary), "-1"], capture_output=True, text=True, check=True
        ).stdout
        for line in out.splitlines():
            m = LINE_RE.match(line.strip())
            if m:
                key = f"{m.group(1)} threads={m.group(2)}"
                samples.setdefault(key, []).append(
                    float(m.group(3)) * TO_MS[m.group(4)]
                )
        parse_ms = samples.get("bmqt::UriParser::parse threads=1", [None])[-1]
        print(
            f"run {i + 1}/{runs} done: parse threads=1 {parse_ms} ms",
            file=sys.stderr,
            flush=True,
        )
    if not samples:
        raise RuntimeError("no benchmark lines parsed; is libbenchmark installed?")
    return {k: round(statistics.median(v), 2) for k, v in samples.items()}


def post_slack(payload):
    token = next(
        (
            os.environ[n]
            for n in (
                "SLACK_BOT_TOKEN",
                "SLACK_ONCALL_BOT_TOKEN",
                "COG_GTM_DEMO_SLACK_BOT_TOKEN",
            )
            if os.environ.get(n)
        ),
        None,
    )
    if not token:
        raise RuntimeError("no Slack bot token in environment")
    req = urllib.request.Request(
        "https://slack.com/api/chat.postMessage",
        data=json.dumps(payload).encode(),
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "Authorization": f"Bearer {token}",
        },
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        body = json.load(resp)
    if not body.get("ok"):
        raise RuntimeError(f"Slack error: {body.get('error')}")
    return body["ts"]


def build_payload(channel, metric, *, base, cur, ratio, results, baseline):
    head = git("rev-parse", "--short=12", "HEAD")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    good = baseline["baseline_sha"][:12]
    rows = "\n".join(
        f"{k:<34} {baseline['results_ms'].get(k, float('nan')):>8.1f} ms  ->"
        f" {v:>8.1f} ms  ({v / baseline['results_ms'][k]:.1f}x)"
        for k, v in sorted(results.items())
        if k in baseline["results_ms"]
    )
    title = (
        f":rotating_light: PERF REGRESSION bmqt::UriParser::parse — {ratio:.1f}x slower"
    )
    when = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    fields = [
        ("Service", "BlazingMQ client/broker — `bmqt::Uri` parsing"),
        ("Metric", f"`{metric}` (100k parses)"),
        ("Baseline → current", f"*{base:.1f} ms → {cur:.1f} ms* ({ratio:.1f}x)"),
        ("Threshold", f"{baseline['threshold']:.1f}x"),
        ("Repo / branch", f"COG-GTM/blazingmq `{branch}`"),
        ("Commit range", f"`{good}..{head}`"),
        ("Host", socket.gethostname()),
        ("Detected", when),
    ]
    return {
        "channel": channel,
        "text": f"{title} | COG-GTM/blazingmq {branch} {good}..{head}",
        "blocks": [
            {
                "type": "header",
                "text": {"type": "plain_text", "text": title[:150], "emoji": True},
            },
            {
                "type": "section",
                "fields": [
                    {"type": "mrkdwn", "text": f"*{k}*\n{v}"} for k, v in fields
                ],
            },
            {"type": "section", "text": {"type": "mrkdwn", "text": f"```{rows}```"}},
            {
                "type": "context",
                "elements": [
                    {
                        "type": "mrkdwn",
                        "text": "Source: `bin/demo/bench_alert.py` nightly perf gate · runbook: AGENTS.md "
                        "→ Perf regression triage · _simulated regression on the COG-GTM demo fork_",
                    }
                ],
            },
        ],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "blazingmq"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--threshold", type=float, default=None)
    ap.add_argument(
        "--dry-run", action="store_true", help="print the Slack payload, do not post"
    )
    ap.add_argument("--no-alert", action="store_true", help="only print results")
    ap.add_argument(
        "--write-baseline",
        action="store_true",
        help="record results as the new baseline",
    )
    args = ap.parse_args()

    binary = Path(args.build_dir) / "tests" / "bmqt_uri.t"
    if not binary.exists():
        sys.exit(f"benchmark binary not found: {binary} (build first)")

    results = run_benchmark(binary, args.runs)
    print(json.dumps(results, indent=2))

    if args.write_baseline:
        BASELINE.write_text(
            json.dumps(
                {
                    "baseline_sha": git("rev-parse", "HEAD"),
                    "metric": "bmqt::UriParser::parse threads=1",
                    "threshold": args.threshold or 1.5,
                    "runs": args.runs,
                    "recorded": datetime.now(timezone.utc).isoformat(
                        timespec="seconds"
                    ),
                    "results_ms": results,
                },
                indent=4,
            )
            + "\n"
        )
        print(f"baseline written to {BASELINE}")
        return 0

    baseline = json.loads(BASELINE.read_text())
    threshold = args.threshold or baseline["threshold"]
    baseline["threshold"] = threshold
    metric = baseline["metric"]
    base, cur = baseline["results_ms"][metric], results[metric]
    ratio = cur / base
    verdict = "REGRESSION" if ratio >= threshold else "OK"
    print(
        f"{metric}: baseline {base:.1f} ms, current {cur:.1f} ms, {ratio:.2f}x -> {verdict}"
    )
    if verdict == "OK" or args.no_alert:
        return 2 if verdict == "REGRESSION" else 0

    payload = build_payload(
        os.environ.get("SLACK_ALERT_CHANNEL", "C0C6T4QNT2R"),
        metric,
        base=base,
        cur=cur,
        ratio=ratio,
        results=results,
        baseline=baseline,
    )
    if args.dry_run:
        print(json.dumps(payload, indent=2))
    else:
        print(f"alert posted to Slack, ts={post_slack(payload)}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
