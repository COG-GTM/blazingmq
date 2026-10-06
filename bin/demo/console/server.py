#!/usr/bin/env python3
"""Perf Gate console: a one-button web UI that runs bin/demo/bench-alert.sh and
follows the Slack alert thread (demo tooling, COG-GTM fork only).

Usage: python3 bin/demo/console/server.py [--port 8787]
Slack posting/reading uses SLACK_ONCALL_BOT_TOKEN (never sent to the browser).
"""
import argparse
import json
import os
import re
import subprocess
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BASELINE = HERE.parent / "baseline.json"
CHANNEL = os.environ.get("SLACK_ALERT_CHANNEL", "C0C6T4QNT2R")
RUN_RE = re.compile(r"run (\d+)/(\d+) done: parse threads=1 ([\d.]+) ms")
VERDICT_RE = re.compile(r"current ([\d.]+) ms, ([\d.]+)x -> (\w+)")
TS_RE = re.compile(r"alert posted to Slack, ts=([\d.]+)")

state_lock = threading.Lock()
state = {"status": "idle"}


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                          text=True).stdout.strip()


def run_gate(mode):
    cmd = [str(HERE.parent / "bench-alert.sh")]
    if mode == "dry":
        cmd.append("--dry-run")
    with state_lock:
        state.clear()
        state.update(status="running", mode=mode, runs=[], log=[], started=time.time(),
                     branch=git("rev-parse", "--abbrev-ref", "HEAD"),
                     head=git("rev-parse", "--short=12", "HEAD"))
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in proc.stdout:
        line = line.rstrip()
        with state_lock:
            if len(state["log"]) < 400:
                state["log"].append(line)
            if m := RUN_RE.search(line):
                state["runs"].append(float(m.group(3)))
                state["total_runs"] = int(m.group(2))
            if m := VERDICT_RE.search(line):
                state.update(current=float(m.group(1)), ratio=float(m.group(2)),
                             verdict=m.group(3))
            if m := TS_RE.search(line):
                state["alert_ts"] = m.group(1)
    rc = proc.wait()
    with state_lock:
        state["status"] = "done" if rc in (0, 2) else "error"
        state["finished"] = time.time()


def slack_replies(ts):
    token = os.environ.get("SLACK_ONCALL_BOT_TOKEN") or os.environ.get("SLACK_BOT_TOKEN")
    if not token:
        return {"error": "no Slack token in environment"}
    q = urllib.parse.urlencode({"channel": CHANNEL, "ts": ts, "limit": 100})
    req = urllib.request.Request("https://slack.com/api/conversations.replies?" + q,
                                 headers={"Authorization": f"Bearer {token}"})
    data = json.load(urllib.request.urlopen(req, timeout=10))
    if not data.get("ok"):
        return {"error": data.get("error")}
    out = []
    for msg in data.get("messages", [])[1:]:
        who = (msg.get("bot_profile") or {}).get("name") or msg.get("username") \
            or ("Devin" if msg.get("bot_id") else msg.get("user", "user"))
        out.append({"ts": msg["ts"], "who": who, "bot": bool(msg.get("bot_id")),
                    "text": msg.get("text", "")})
    return {"messages": out}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/":
            return self.send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if url.path == "/api/config":
            base = json.loads(BASELINE.read_text())
            return self.send(200, {"baseline": base["results_ms"][base["metric"]],
                                   "threshold": base["threshold"], "metric": base["metric"],
                                   "baseline_sha": base["baseline_sha"][:12], "channel": CHANNEL})
        if url.path == "/api/status":
            with state_lock:
                return self.send(200, dict(state))
        if url.path == "/api/thread":
            ts = urllib.parse.parse_qs(url.query).get("ts", [""])[0]
            if not re.fullmatch(r"[\d.]+", ts):
                return self.send(400, {"error": "bad ts"})
            try:
                return self.send(200, slack_replies(ts))
            except Exception as e:  # network hiccups should not kill the UI
                return self.send(200, {"error": str(e)})
        self.send(404, {"error": "not found"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/run":
            mode = urllib.parse.parse_qs(url.query).get("mode", ["live"])[0]
            with state_lock:
                if state.get("status") == "running":
                    return self.send(409, {"error": "already running"})
                state["status"] = "running"
            threading.Thread(target=run_gate, args=(mode,), daemon=True).start()
            return self.send(202, {"ok": True})
        self.send(404, {"error": "not found"})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    print(f"Perf Gate console on http://{args.host}:{args.port}")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
