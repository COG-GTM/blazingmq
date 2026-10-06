#!/usr/bin/env bash
# Demo perf gate: runs the bmqt URI parser benchmark 5x, compares the median to
# bin/demo/baseline.json and posts a Slack alert on a >=1.5x regression.
# All flags are forwarded to bench_alert.py (e.g. --dry-run, --no-alert).
set -euo pipefail
exec python3 "$(dirname "$0")/bench_alert.py" "$@"
