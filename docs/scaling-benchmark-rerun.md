# Scaling Benchmark Rerun Commands

Use the perf build and run one server instance with logging disabled. `TERMCHAT_IDLE_TIMEOUT_MS` defaults to `180000`
in the current server; keep it above the benchmark duration when validating whether previous EOF failures were caused
by the idle timer:

```bash
cmake --preset perf
cmake --build build/relwithdebinfo
TERMCHAT_IDLE_TIMEOUT_MS=300000 ./build/relwithdebinfo/server/src/server -l 0
```

The server prints the effective `TERMCHAT_IDLE_TIMEOUT_MS` on startup. Confirm that value before starting a rerun.

Keep `-connect-ramp` below the idle timeout so early clients do not finish warmup and then sit idle long enough to be
closed before the measured phase starts.

This rerun changes `02-scaling` from a short burst test to an IM-like online-user test:

- More concurrent long-lived TCP clients than the earlier 2,000-2,300 client burst runs.
- Lower per-user message frequency with `-duration` and `-rate-per-client`.
- Random online receiver selection with `-receiver-mode random-online`, avoiding the single hot receiver used by the earlier scaling runs.
- `-inflight 1` because rate mode sends at a controlled per-client pace.
- `-warmup 5` keeps a small pre-measurement warmup, and warmup messages use the same random online receiver mode as the measured phase.
- `-rate-schedule poisson` avoids synchronized send waves across all clients while preserving the configured average per-client message rate. Rate-mode clients share one global benchmark deadline, so high client counts do not stretch the measured window because of benchmark-driver scheduling lag.
- `-request-timeout 30s` bounds request-level ACK waits, so benchmark failures are reported as client-observed request timeouts instead of waiting for the server idle timer.
- The benchmark sends unmeasured keepalive traffic while clients wait at the final start barrier, so early clients are not closed by the server idle timeout during large connection ramps.

These runs should be interpreted as online-capacity and latency-stability data. Do not compare their `success_qps` directly with the earlier `inflight=2` burst runs.

## Single Pass

3,000 online clients, one message every 5 seconds per client on average:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 3000 \
  -duration 180s \
  -rate-per-client 0.2 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_3000c_0_2rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-3000c
```

4,000 online clients, one message every 5 seconds per client on average:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 4000 \
  -duration 180s \
  -rate-per-client 0.2 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_4000c_0_2rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-4000c
```

5,000 online clients, one message every 5 seconds per client on average:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 5000 \
  -duration 180s \
  -rate-per-client 0.2 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_5000c_0_2rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-5000c
```

If the 5,000-client run is stable and you want to probe headroom, increase connected users while keeping per-user rate fixed:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 8000 \
  -duration 180s \
  -rate-per-client 0.2 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_8000c_0_2rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-8000c
```

## Lower-Frequency Capacity Probe

If the benchmark driver or local file descriptor limit becomes the bottleneck before the server does, reduce per-client traffic to one message every 20 seconds and increase connection count:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 10000 \
  -duration 240s \
  -rate-per-client 0.05 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_10000c_0_05rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-10000c
```

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 20000 \
  -duration 240s \
  -rate-per-client 0.2 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_20000c_0_2rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-20000c
```

## Three-Pass Template

Use distinct output directories when collecting repeat runs:

```bash
for run in 1 2 3; do
  go run ./tests/perf \
    -addr 127.0.0.1:1316 \
    -clients 5000 \
    -duration 180s \
    -rate-per-client 0.2 \
    -rate-schedule poisson \
    -request-timeout 30s \
    -payload 256 \
    -inflight 1 \
    -warmup 5 \
    -receiver-mode random-online \
    -connect-ramp 45s \
    -scenario im_scaling_5000c_0_2rps_random_ramp45s_run${run} \
    -out benchmark-results/02-scaling-im-5000c-run${run}
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
duration_seconds
rate_per_client
rate_schedule
request_timeout_ms
receiver_mode
```
