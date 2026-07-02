# divinus tests

Self-contained test tree with its own build, kept out of `src/` so it stays
easy to maintain and upstream. Three tiers:

| Tier | Location | Runs in CI | How to run locally |
|---|---|---|---|
| Unit (host-native) | `tests/unit/` | yes | `make -C tests` |
| Static analysis | `tests/analyze.sh` | yes | `sh tests/analyze.sh` |
| Integration (live camera) | `tests/integration/` | no (needs hardware) | see below |

Only the vendor-free, hardware-free sources are host-testable: `src/fmt/`
(NAL, MP4/fMP4, FLV, bitbuf), `src/rtsp/` (RTP, RTSP), `server.c`,
`app_config.c`. `src/hal/` is hardware-bound and `src/lib/` is vendored
third-party — both stay out of host tests and analysis.

## Unit tests

```sh
make -C tests          # build + run every tests/unit/test_*.c
make -C tests clean
```

Each test compiles natively with `-Wall -Wextra -fsanitize=address,undefined`
and lists the `src/` objects it needs via a `<name>_SRC` variable in the
Makefile. `tests/unit/test_nal.c` is a smoke test that proves the toolchain;
real coverage comes later.

## Static analysis

```sh
sh tests/analyze.sh
```

Runs `gcc -fanalyzer` over `src/` (excluding `src/lib/`) and fails on any
analyzer finding that is not already in `tests/analyze-baseline.txt`. The
baseline records pre-existing findings so the gate catches regressions on
every PR; once a baselined finding is fixed, delete
its line from the baseline. CodeQL default setup (repo Settings → Code
security) covers the same class with zero maintenance.

## Integration (live camera)

`tests/integration/` holds the on-target suites; they need a reachable camera
and are run by hand before shipping any stream-path change.

```sh
uv run tests/integration/test_streams.py <camera-ip>      # stream integrity
uv run tests/integration/measure_latency.py <camera-ip>   # latency + cadence
```

### `test_streams.py` run modes

The suite has two groups: **stream-integrity** checks (MJPEG/RTSP/fMP4 +
runtime UDP push) and **exposure** checks that drive fps/exposure/gain live.
The exposure tests mutate sensor state, so they run only on request — verifying
a HAL/stream change should not need them:

```sh
uv run .../test_streams.py <ip>                 # streams only (default; no exposure mutation)
uv run .../test_streams.py <ip> all             # streams + exposure
uv run .../test_streams.py <ip> exposure        # exposure tests only
uv run .../test_streams.py <ip> --no-exposure   # explicit streams-only (== default)
uv run .../test_streams.py <ip> rtsp-tcp fmp4-http   # explicit subset by test name
```

Each exposure test has a `try/finally` teardown (`restore_streaming`) that puts
fps/exposure/gain back to a known-good state and confirms frames flow again,
even when an assertion fails mid-run — so a failed exposure test no longer
leaves the camera wedged ("Main stream loop timed out") for the next probe.
