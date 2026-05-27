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

Use `-inflight 1` for the original sequential request/ACK baseline. Use a larger value such as `-inflight 64` to measure single-connection protocol pipelining, where multiple requests are sent before their ACKs return.

The output directory is stable. If `-out` is omitted, it defaults to `benchmark-results/<scenario>`; running the same scenario again overwrites the files in that directory instead of creating a timestamped folder.

`go.mod` and `go.sum` intentionally stay at the repository root because the benchmark imports generated protobuf packages under `build/relwithdebinfo/proto/go` using the root Go module path.

See [Performance Engineering](../../docs/performance-engineering.md) for the `PERSISTED` ACK push switch, A/B benchmark commands, and perf/FlameGraph workflow.
