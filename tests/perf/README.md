# TermChat Performance Benchmarks

Go is used as the benchmark driver because it can maintain many concurrent TCP clients with lower overhead than Python scripts. Python should remain responsible for functional tests and offline report analysis.

Run the current single-connection baseline after generating Go protobuf files:

```bash
cmake --preset perf
cmake --build build/relwithdebinfo
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -n 10000 \
  -payload 256 \
  -inflight 1 \
  -out benchmark-results/dev-single-conn
```

Each run writes:

```text
scenario.json
summary.json
latency.csv
report.md
```

Use `-inflight 1` for the original sequential request/ACK baseline. Use a larger value such as `-inflight 64` to measure single-connection protocol pipelining, where multiple requests are sent before their ACKs return. Use `-clients` and `-messages-per-client` to run multiple independent TCP benchmark clients concurrently:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 100 \
  -messages-per-client 1000 \
  -payload 256 \
  -inflight 16 \
  -scenario multi_client_100_inflight_16 \
  -out benchmark-results/multi-client-100-inflight-16
```

For large multi-client runs, use `-connect-ramp` to spread connection, login, and warmup startup across a fixed window. The measured benchmark phase still starts only after all clients are ready:

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

Use `-duration` and `-rate-per-client` for IM-like low-frequency traffic. Rate mode currently uses `-inflight 1` so each client sends at a controlled per-client message rate:

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

The output directory is stable. If `-out` is omitted, it defaults to `benchmark-results/<scenario>`; running the same scenario again overwrites the files in that directory instead of creating a timestamped folder.

`go.mod` and `go.sum` intentionally stay at the repository root because the benchmark imports generated protobuf packages under `build/relwithdebinfo/proto/go` using the root Go module path.

See [Performance Engineering](../../docs/performance-engineering.md) for the `PERSISTED` ACK push switch, A/B benchmark commands, and perf/FlameGraph workflow.
