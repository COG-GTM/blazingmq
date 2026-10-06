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
#
# Demo helper: profiles the URI benchmark with `perf` and prints where the
# time goes, with C++ template arguments collapsed so it fits on one screen.
#
#   bin/demo/show.sh "Investigate: profile" bin/demo/profile.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
data=/tmp/demo-perf.data
sudo rm -f "${data}"
echo "perf record -g: build/blazingmq/tests/bmqt_uri.t -1 ..."
sudo perf record -q -g -o "${data}" \
    "${root}/build/blazingmq/tests/bmqt_uri.t" -1 >/dev/null 2>&1
sudo chmod a+r "${data}"
printf '\n%% of CPU cycles (including callees)\n\n'
perf report -i "${data}" --stdio --children -g none --sort symbol \
    --percent-limit 5 2>/dev/null |
    grep -v -e '^#' -e '^$' |
    sed -E ':a;s/<[^<>]*>//g;ta' |
    grep -E 'UriParser::parse|Uri::Uri\(|regex|_Executor::_M_(match|dfs)\b|std::function::operator' |
    sed -E 's/^ *([0-9.]+%) +[0-9.]+% +\[\.\] /  \1  /' |
    cut -c1-110
