# Scaling Benchmark Rerun Commands

Use the perf build and run one server instance with logging disabled:

```bash
cmake --preset perf
cmake --build build/relwithdebinfo
./build/relwithdebinfo/server/src/server -l 0
```

The server currently uses a 60-second idle timeout. Keep `-connect-ramp` below that timeout so early clients do not finish warmup and then sit idle long enough to be closed before the measured phase starts.

## Single Pass

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2000 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2000c_i2_ramp30s \
  -out benchmark-results/02-scaling-2000c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2050 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2050c_i2_ramp30s \
  -out benchmark-results/02-scaling-2050c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2100 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2100c_i2_ramp30s \
  -out benchmark-results/02-scaling-2100c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2150 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2150c_i2_ramp30s \
  -out benchmark-results/02-scaling-2150c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2200 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2200c_i2_ramp30s \
  -out benchmark-results/02-scaling-2200c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2250 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2250c_i2_ramp30s \
  -out benchmark-results/02-scaling-2250c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 2300 \
  -messages-per-client 200 \
  -payload 256 \
  -inflight 2 \
  -connect-ramp 30s \
  -scenario scaling_2300c_i2_ramp30s \
  -out benchmark-results/02-scaling-2300c
```

## Three-Pass Template

Use distinct output directories when collecting repeat runs:

```bash
for run in 1 2 3; do
  go run ./tests/perf \
    -addr 127.0.0.1:1316 \
    -clients 2100 \
    -messages-per-client 200 \
    -payload 256 \
    -inflight 2 \
    -connect-ramp 30s \
    -scenario scaling_2100c_i2_ramp30s_run${run} \
    -out benchmark-results/02-scaling-2100c-run${run}
done
```

Only use runs with `valid: true` and `errors: {}` for capacity conclusions. Compare:

```text
valid
errors
success_qps
latency_ms.p95
latency_ms.p99
latency_ms.p999
connect_ramp_seconds
```
