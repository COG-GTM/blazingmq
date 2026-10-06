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
# screen recording of the triage shows it, waits for it to finish, and exits
# with the command's exit code. Output is also appended to
# /tmp/demo-show.log so the caller can read it.
#
#   bin/demo/show.sh "Bisect" "git bisect run /tmp/bisect-step.sh"
set -euo pipefail
title="$1"
shift
cmd="$*"
root="$(cd "$(dirname "$0")/../.." && pwd)"
rcfile="$(mktemp)"
export DISPLAY="${DISPLAY:-:0}"
xterm -T "BlazingMQ SRE triage: ${title}" -maximized \
    -fa 'DejaVu Sans Mono' -fs "${SHOW_FONT_SIZE:-15}" \
    -bg '#0b1020' -fg '#e6edf3' -sl 5000 \
    -e bash -c "cd '${root}'
printf '\033[1;36m== %s ==\033[0m\n\$ %s\n\n' \"${title}\" \"${cmd//\"/\\\"}\"
( ${cmd} ) 2>&1 | tee -a /tmp/demo-show.log; echo \${PIPESTATUS[0]} > '${rcfile}'
sleep ${SHOW_HOLD_SECONDS:-6}"
rc="$(cat "${rcfile}" 2>/dev/null || echo 1)"
rm -f "${rcfile}"
exit "${rc:-1}"
