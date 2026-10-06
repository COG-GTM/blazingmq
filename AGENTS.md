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

**Film the investigation.** The room watches a video of the triage, so make
the investigation visible on the desktop and record it. The recording tool
auto-edits out idle time and keeps the footage around annotations, so put
the result on screen and annotate it:
- Start `recording_start` (fps 10) only once the build is done and you are
  about to bisect, so the video doesn't open on a blank screen.
- Run every investigation command through `bin/demo/show.sh "<step title>"
  "<command>"`. It opens a large terminal on the desktop, runs the command,
  and returns with the command's exit code (output is also appended to
  `/tmp/demo-show.log`). **The window stays open on the result** until the
  next `show.sh` call or `show.sh --close`.
- After every `show.sh` step, while its result is on screen: call
  `annotate_recording` (`type=setup`) with the finding in one line, e.g.
  "First bad commit ce09e26d: parse 36 → 263 ms (7x)", then `sleep 8`. This
  keeps the result frames in the edited video.
- Show at least these steps, in order:
  1. `Correlate: bisect`: `git bisect start/good/bad`, then `git bisect run`
     with a step script that runs `bench_alert.py --no-alert --threshold 3`
     (exit 2 = bad). The planted regression is 6–9x and single runs have
     about ±15% noise, so the 1.5x alert line is too tight for bisecting.
     Don't pass `--quiet`; let each step's benchmark line print. Finish
     with `git bisect log`.
  2. `Investigate: offending diff`:
     `git --no-pager show --color=always <bad-sha> -- src/groups/bmq/bmqt/bmqt_uri.cpp`.
  3. `Investigate: profile`: `bin/demo/show.sh "Investigate: profile"
     bin/demo/profile.sh`. It runs `sudo perf record -g` on `bmqt_uri.t -1`
     and prints a one-screen "% of cycles" table (`UriParser::parse`,
     `std::regex_match`, the regex `_Executor`), so the regex cost is
     readable on screen.
- Then `show.sh --close`, `recording_stop` with title "BlazingMQ perf
  triage" and a 2-sentence summary. Post the video in the alert thread
  (`post_message` with `file_path`, text "Investigation recording: bisect →
  diff → perf profile"). If it is over 35 MB, post the session link instead.
  Attach the same video to the PR description in step 5.
- Recording is best effort: if it fails, say so in one line and carry on.
  Never let it delay the approval stop.

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
