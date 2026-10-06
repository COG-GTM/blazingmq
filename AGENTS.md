# AGENTS.md — COG-GTM/blazingmq

This is a fork of [bloomberg/blazingmq](https://github.com/bloomberg/blazingmq), a
C++17 message queue built on BDE and ntf-core. It is used for demos. Open PRs
against **COG-GTM/blazingmq** only, never against bloomberg/blazingmq upstream.

## Build and test

```bash
pip install 'cmake==3.31.*'          # the VM ships CMake 3.22; the project needs >= 3.24
sudo apt-get install -y libbenchmark-dev libgmock-dev libgtest-dev zlib1g-dev
bin/build-ubuntu.sh                  # BDE + ntf-core + BlazingMQ into build/ (~8-9 min clean)
cmake --build build/blazingmq -j8    # incremental rebuild after edits
cd build/blazingmq && ctest -R bmqt --output-on-failure -j8   # bmqt unit tests
```

- `build/blazingmq/tests/bmqt_uri.t -1` runs the `bmqt::Uri` benchmark: 100k
  `UriParser::parse` / `Uri` constructions per thread, at 1/2/4/8 threads.
  Each case runs once, so expect about ±15% noise between runs. Compare medians
  of 5 runs.
- `bin/demo/bench_alert.py` runs the benchmark 5 times and compares the medians
  against `bin/demo/baseline.json`.
  - Use `--no-alert` to print the comparison without posting to Slack.
  - Use `--build-dir` to point it at another build.
- Broker: `build/blazingmq/src/applications/bmqbrkr/run` (port 30114).
  Client: `build/blazingmq/src/applications/bmqtool/bmqtool.tsk`.

## Perf regression triage (on-call runbook)

When you are started from a `PERF REGRESSION` alert in #blazingmq-sre-alerts, work the
SRE loop below. Post a short update in the alert's Slack thread at every step.
Keep each update to 3–6 lines, with numbers and SHAs.

1. **Detect / acknowledge.** Reply in the thread. Say you are investigating,
   restate the metric, the baseline vs. current numbers, and the commit range
   from the alert.
2. **Correlate.**
   - Check out the alert's branch.
   - Build once, then bisect the commit range (`git bisect run` with an
     incremental rebuild plus `bench_alert.py --no-alert`).
   - Post the first bad commit with its author, message, and the benchmark
     numbers on either side.
3. **Investigate / RCA.** Read the offending diff and explain the mechanism.
   Post an RCA covering:
   - what changed, and why it is slow;
   - blast radius: every client and broker code path that parses a queue URI,
     e.g. `bmqa::Session::openQueue`, broker queue lookup and admin commands;
   - user impact;
   - whether behaviour or correctness changed.
4. **Recommend.** Propose the minimal fix and say what you will verify.
   Then **stop and ask for SRE approval in the thread.** Do not change code
   until a human replies `approve` (or similar) in the thread. If they push
   back, adjust the plan and ask again.
5. **Remediate.** After approval:
   - Create a branch named `devin/<epoch>-fix-uri-parse-regression` off the
     alert's branch.
   - Fix it in the existing BDE/BlazingMQ style.
   - Add a regression guard: a unit test case in `bmqt_uri.t.cpp` that
     protects the behaviour you changed, plus a coarse perf check where it is
     sensible.
   - Open a PR **into the alert's branch** on COG-GTM/blazingmq. Include the
     RCA, before/after benchmark medians (all thread counts) and test output.
     Link the PR in the thread.
6. **Verify.** Rebuild, run `ctest -R bmqt`, and run
   `bin/demo/bench-alert.sh --no-alert` (wrapper for `bench_alert.py`). Post the final before → after table
   in the thread and on the PR. Then run Devin Review on the PR.

Rules:
- Never merge the PR.
- Never push to `main` or to the alert's branch directly.
- Never touch bloomberg/blazingmq upstream.
- Commit messages must contain "bug" or "feature".
