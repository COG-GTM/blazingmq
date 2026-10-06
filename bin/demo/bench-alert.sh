#!/usr/bin/env bash
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

# Demo perf gate: runs the bmqt URI parser benchmark 5x, compares the median to
# bin/demo/baseline.json and posts a Slack alert on a >=1.5x regression.
# All flags are forwarded to bench_alert.py (e.g. --dry-run, --no-alert).
set -euo pipefail
exec python3 "$(dirname "$0")/bench_alert.py" "$@"
