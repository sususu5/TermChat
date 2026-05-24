# TermChat Performance Benchmarks

Go is used as the benchmark driver because it can maintain many concurrent TCP clients with lower overhead than Python scripts. Python should remain responsible for functional tests and offline report analysis.

Run the current single-connection baseline after generating Go protobuf files with the debug preset:

```bash
cmake --preset debug
cmake --build build/debug --target proto_generated
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -n 10000 \
  -payload 256 \
  -out benchmark-results/dev-single-conn
```

Each run writes:

```text
scenario.json
summary.json
latency.csv
report.md
```

`go.mod` and `go.sum` intentionally stay at the repository root because the benchmark imports generated protobuf packages under `build/debug/proto/go` using the root Go module path.
