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
real coverage is added by the unit-test cards.

## Static analysis

```sh
sh tests/analyze.sh
```

Runs `gcc -fanalyzer` over `src/` (excluding `src/lib/`) and fails on any
analyzer finding that is not already in `tests/analyze-baseline.txt`. The
baseline records pre-existing findings so the gate catches regressions on
every PR; as each baselined finding is fixed (tracked by its own card), delete
its line from the baseline. CodeQL default setup (repo Settings → Code
security) covers the same class with zero maintenance.

## Integration (live camera)

`tests/integration/` holds the on-target suites; they need a reachable camera
and are run by hand before shipping any stream-path change.

```sh
uv run tests/integration/test_streams.py <camera-ip>      # stream integrity
uv run tests/integration/measure_latency.py <camera-ip>   # latency + cadence
```
