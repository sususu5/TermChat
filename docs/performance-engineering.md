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

For protocol pipelining tests, keep the same server mode and increase benchmark in-flight requests on one TCP connection:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -n 10000 \
  -payload 256 \
  -inflight 64 \
  -scenario single_conn_inflight_64 \
  -out benchmark-results/single-conn-inflight-64
```

`-inflight 1` is the sequential baseline. Larger values test whether the protocol and server can correlate concurrent requests by `Envelope.seq` without forcing a full RTT wait between messages.

For multi-client tests, run independent TCP clients concurrently and keep per-connection in-flight bounded:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 1000 \
  -messages-per-client 1000 \
  -payload 256 \
  -inflight 16 \
  -scenario multi_client_1000_inflight_16 \
  -out benchmark-results/multi-client-1000-inflight-16
```

For high client counts, add a connection ramp so the benchmark does not measure a synthetic connect/login storm. Ramp-up covers connection establishment, login, and warmup; the measured phase still starts after all clients are ready:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2100 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2100c_i2 \
  -out benchmark-results/02-scaling-2100c
```

For IM-like low-frequency traffic, run many connected clients for a fixed duration with a target per-client send rate:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 1000 \
  -duration 120s \
  -rate-per-client 0.1 \
  -payload 256 \
  -inflight 1 \
  -connect-ramp 30s \
  -scenario im_1000_users_0_1rps \
  -out benchmark-results/im-1000-users-0-1rps
```

This represents 1000 online clients with an average target of one message every 10 seconds per client. Use it to evaluate latency stability and error rate under realistic IM traffic instead of peak send-to-ACK throughput only.

Compare these fields in `summary.json`:

```text
valid
success_qps
skipped_pushes
latency_ms.p99
latency_ms.p999
errors
connect_ramp_seconds
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
