# Performance Engineering

TermChat benchmark results should be compared under the same build type, server flags, database state, and benchmark scenario. DevContainer results are useful for trend analysis and regression checks, but not final capacity claims.

## Build Modes

Use the debug preset for development and correctness checks:

```bash
cmake --preset debug
cmake --build build/debug
```

Use the perf preset for manual performance runs:

```bash
cmake --preset perf
cmake --build build/relwithdebinfo
```

The perf preset builds with optimization while keeping debug symbols, so it usually performs better than `debug` and still works with `perf` and FlameGraph. It is the preferred local build for profiling. The Go benchmark imports protobuf code generated under `build/relwithdebinfo/proto/go`, so run this build before manual benchmark tests.

## PERSISTED ACK Push Switch

By default, the server does not push one `ACK_STATUS_PERSISTED` event for every message. The message is still persisted and dedup state is still updated; only the real-time persisted notification is suppressed to reduce ACK amplification.

Default performance mode:

```bash
./build/relwithdebinfo/server/src/server -l 0
```

Full ACK mode:

```bash
TERMCHAT_PUSH_PERSISTED_ACK=1 ./build/relwithdebinfo/server/src/server -l 0
```

`ENABLE_PERSISTED_ACK_PUSH=1` is also accepted for compatibility.

## Manual A/B Benchmark

Run one server at a time. Use separate stable output directories for each scenario. Re-running the same command refreshes `scenario.json`, `summary.json`, `latency.csv`, and `report.md` in that directory.

Default mode:

```bash
./build/relwithdebinfo/server/src/server -l 0
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -n 10000 \
  -payload 256 \
  -out benchmark-results/persisted-ack-off
```

Full ACK mode:

```bash
TERMCHAT_PUSH_PERSISTED_ACK=1 ./build/relwithdebinfo/server/src/server -l 0
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -n 10000 \
  -payload 256 \
  -out benchmark-results/persisted-ack-on
```

Compare these fields in `summary.json`:

```text
valid
success_qps
skipped_pushes
latency_ms.p99
latency_ms.p999
errors
```

`skipped_pushes` counts async server events that arrived while the benchmark was waiting for the request-response ACK. Lower values mean less event interleaving on the critical send-to-ACK path.

## Profiling

Use the perf build and keep the server running:

```bash
perf record -F 99 -p $(pgrep server) -g -- sleep 120
perf script | stackcollapse-perf.pl | flamegraph.pl > perf.svg
```

When comparing flamegraphs, use the same scenario and message count. A valid run should have `valid: true` and `errors: {}` in `summary.json`.

## Current DevContainer Reference

Single-connection sequential benchmark, 10,000 messages, 256-byte payload:

| Mode | success_qps | skipped_pushes | p99 | p999 |
| --- | ---: | ---: | ---: | ---: |
| `TERMCHAT_PUSH_PERSISTED_ACK=1` | 1423.82 | 10097 | 2.352 ms | 41.167 ms |
| default | 1600.94 | 0 | 2.657 ms | 8.651 ms |

These numbers are DevContainer trend data. Use them to validate the effect of ACK reduction, not as final production capacity.
