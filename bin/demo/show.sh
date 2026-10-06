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
# Demo helper: runs a command in a large, maximized xterm on the desktop so a
# screen recording of the triage shows it. Returns when the command finishes,
# with the command's exit code, but leaves the window open on the result (the
# next show.sh call, or `show.sh --close`, replaces it). Output is also
# appended to /tmp/demo-show.log so the caller can read it.
#
#   bin/demo/show.sh "Correlate: bisect" "git bisect run /tmp/bisect-step.sh"
#   bin/demo/show.sh --close
set -euo pipefail
marker="BlazingMQ SRE triage:"
for pid in $(pgrep -x xterm || true); do
    if grep -qa "${marker}" "/proc/${pid}/cmdline" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
    fi
done
if [ "${1:-}" = "--close" ]; then
    exit 0
fi
title="$1"
shift
cmd="$*"
root="$(cd "$(dirname "$0")/../.." && pwd)"
rcfile="$(mktemp -u)"
export DISPLAY="${DISPLAY:-:0}"
setsid xterm -T "${marker} ${title}" -maximized \
    -fa 'DejaVu Sans Mono' -fs "${SHOW_FONT_SIZE:-15}" \
    -bg '#0b1020' -fg '#e6edf3' -sl 5000 \
    -e bash -c "cd '${root}'
printf '\033[1;36m== %s ==\033[0m\n\$ %s\n\n' \"${title}\" \"${cmd//\"/\\\"}\"
( ${cmd} ) 2>&1 | tee -a /tmp/demo-show.log; rc=\${PIPESTATUS[0]}
printf '\n\033[1;33m[exit %s]\033[0m\n' \$rc; echo \$rc > '${rcfile}'
exec sleep infinity" </dev/null >/dev/null 2>&1 &
while [ ! -s "${rcfile}" ]; do
    if ! kill -0 $! 2>/dev/null; then
        echo "show.sh: xterm exited early" >&2
        exit 1
    fi
    sleep 1
done
rc="$(cat "${rcfile}")"
rm -f "${rcfile}"
exit "${rc:-1}"
