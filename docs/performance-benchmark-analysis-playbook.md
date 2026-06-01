# TermChat Performance Benchmark Analysis Playbook

This document records the benchmark scenarios, result interpretation rules, failure analysis process, and tuning
strategies used while evaluating TermChat as a high-concurrency IM server.

## Goals

The benchmark work has two separate goals:

- Establish a stable, resume-friendly IM performance number.
- Find the single-node pressure boundary for future server-side tuning.

Do not mix these two goals. A peak burst result can be useful for engineering exploration, but a resume number should
come from a realistic and stable scenario with no request failures.

## Scenario Design

The preferred IM-like scenario is many long-lived connections with low per-user message frequency:

```text
clients: many online users
rate-per-client: low per-user send frequency
receiver-mode: random-online
rate-schedule: poisson
inflight: 1
warmup: enabled
connect-ramp: enabled
request-timeout: bounded, for example 30s
```

This is closer to a real IM workload than a small number of clients sending messages as fast as possible. IM systems
usually have many online users, sparse sends per user, random recipients, and bursty aggregate traffic. The old
`inflight=2` peak burst data is useful as a stress test, but it should not be presented as the primary business-like
capacity result.

Recommended local scenarios:

```text
20,000 clients * 0.05 msg/s = about 1,000 msg/s
20,000 clients * 0.10 msg/s = about 2,000 msg/s
20,000 clients * 0.20 msg/s = about 4,000 msg/s pressure boundary
```

For local DevContainer testing, start with the stable capacity line, then increase throughput. For cloud testing, use
separate load-generator and server machines before trying 30k or 50k connections.

## Benchmark Parameters

Use `random-online` receivers:

```text
-receiver-mode random-online
```

This avoids a single hot receiver and better represents normal private-message traffic.

Use Poisson scheduling:

```text
-rate-schedule poisson
```

This avoids synchronized send waves from thousands of clients while preserving the configured average per-user send
rate.

Keep `inflight=1` for the IM scenario:

```text
-inflight 1
```

Higher inflight values measure pipelining and burst capability, not normal low-frequency user behavior.

Keep warmup enabled:

```text
-warmup 5
```

Warmup is necessary for enterprise-style measurement because it avoids cold-start artifacts. Warmup messages should use
the same random receiver mode as the measured phase.

Use a bounded request timeout:

```text
-request-timeout 30s
```

This prevents the benchmark from waiting indefinitely and makes request-level failures visible. A larger timeout such
as `60s` can be used only for diagnosis, not as the preferred official measurement.

Use a connection ramp:

```text
-connect-ramp 45s
```

This reduces connection storm effects. Keep the server idle timeout above the full setup and benchmark duration.

Recommended server startup for the current local benchmark pass:

```bash
TERMCHAT_IDLE_TIMEOUT_MS=300000 TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1 ./build/relwithdebinfo/server/src/server -l 0
```

Use this mode when measuring the optimized ACK hot path. It disables synchronous `client_msg_id` dedup storage in front
of the immediate ACK, but keeps async message persistence enabled. For a production-equivalent reliability benchmark,
run without `TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1` or implement a fast cache/asynchronous dedup replacement first.

## Result Interpretation

Prefer measurement-window QPS:

```text
Attempted QPS
Success QPS
```

Do not use end-to-end QPS as the main throughput number, because it includes connection ramp, login, warmup, and
teardown time.

For rate-mode benchmarks, `completed_messages` may be slightly higher or lower than `requested_messages`. With Poisson
scheduling, the exact number of sends is random around the expected value.

Required fields to inspect:

```text
valid
errors
success
completed_messages
requested_messages
attempted_qps
success_qps
latency_ms.p95
latency_ms.p99
latency_ms.p999
latency_ms.max
skipped_pushes
skipped_stale_acks
```

For final capacity claims, prefer `valid: true` and no official measurement failures. The benchmark may still report
non-measurement recovery events such as `warmup_timeout=1`; those should be disclosed or investigated, but they are not
the same as failed measured requests when `summary.valid` is true and `latency.csv` has no failed rows. A result with
very high success rate but `valid: false` can be used for pressure-boundary analysis, not as the main resume result.

## Known Result Classes

Stable capacity result:

```text
20,000 long-lived connections
0.05 msg/s per client
about 1,000 msg/s aggregate
100% success
p99 around 160ms in local testing
```

This is the best current resume-style performance line.

Pressure result:

```text
20,000 long-lived connections
0.10 msg/s per client
about 2,000 msg/s aggregate
rare timeouts
p99 and p999 higher than the stable line
```

This shows the system is near a local pressure boundary or affected by local benchmark environment noise.

Boundary exploration:

```text
20,000 long-lived connections
0.20 msg/s per client
about 4,000 msg/s aggregate
tail latency can reach seconds
rare failures observed
```

This is useful for tuning, but not as the primary external performance claim.

Failed local overload attempt:

```text
30,000 clients * 0.2 msg/s
signal: killed
```

`signal: killed` means the Go benchmark process was killed by the OS, most likely because of container memory pressure
or OOM. It is not evidence that the server reached its true limit.

## Failure Types

### EOF During Warmup Or Measurement

Typical symptom:

```text
read length failed: EOF
```

Likely causes:

- Server closed idle clients before measurement started.
- Server idle timeout was too low.
- Client waited too long at a barrier without keepalive.
- Server or benchmark process restarted.

Mitigations:

- Increase `TERMCHAT_IDLE_TIMEOUT_MS`.
- Keep `connect-ramp` below idle timeout.
- Send keepalive traffic while clients wait at barriers.
- Reconnect clients after non-measurement warmup or pre-start failures.

### Request Timeout

Typical symptom:

```text
read length failed: read tcp 127.0.0.1:xxxxx->127.0.0.1:1316: i/o timeout
```

Meaning:

- The benchmark client sent a request.
- It did not receive the expected ACK within `-request-timeout`.
- This is a real client-observed failure unless a later stale ACK proves it was only delayed.

Observed examples at `20k * 0.1 msg/s`:

```text
timeout=3 or timeout=4
success rate above 99.999%
skipped_stale_acks sometimes > 0
```

Interpretation:

- The system has extreme tail latency in this scenario.
- At least some requests are not lost; their ACK arrives after timeout and is counted as stale.
- Results with any timeout are not `valid: true`.

### Unknown Errors

Earlier runs showed:

```text
unknown=N
unexpected response: seq=X cmd=CMD_MSG_ACK, expected seq=Y cmd=CMD_MSG_ACK
```

Root cause:

- One request timed out.
- Its late ACK remained in the socket.
- The next request read that old ACK and reported an unexpected sequence.
- One real timeout could cascade into many protocol-looking failures.

Benchmark fix:

- If waiting for ACK `seq=N`, skip same-command ACKs with `seq<N`.
- Count them as `skipped_stale_acks`.
- Classify unexpected responses separately instead of `unknown`.

### Warmup Timeout

Typical symptom:

```text
warmup_timeout=1
```

Meaning:

- A non-measured warmup request timed out.
- It should still be recorded because it indicates environment or server instability.

If reconnect succeeds before measurement, this may not invalidate the measured records by itself, but it should be
treated as a warning.

## Monitoring

Use the monitoring wrapper:

```bash
./scripts/run_perf_with_monitor.sh
```

It records:

```text
benchmark-results/02-scaling-im-20000c/monitor/bench.log
benchmark-results/02-scaling-im-20000c/monitor/top.log
benchmark-results/02-scaling-im-20000c/monitor/vmstat.log
benchmark-results/02-scaling-im-20000c/monitor/ss-summary.log
benchmark-results/02-scaling-im-20000c/monitor/ss-established.log
benchmark-results/02-scaling-im-20000c/monitor/meta.log
```

`ss` comes from the `iproute2` package. The DevContainer should include `iproute2`, and the script also has a
`/proc/net` fallback.

What to check:

- `top.log`: server CPU, benchmark client CPU, memory, swap pressure, unrelated noisy processes.
- `vmstat.log`: run queue, blocked tasks, context switches, interrupts, IO wait, swap in/out.
- `ss-summary.log`: established sockets, closed sockets, TIME_WAIT growth, orphaned sockets.
- `ss-established.log`: whether connections reached and held the target count.

On loopback, `ss` can count both client-side and server-side sockets. For 20,000 TCP connections, seeing about 40,000
established endpoints is normal.

## Diagnosing 20k Client Failures

For a run like:

```bash
go run ./tests/perf \
  -addr 127.0.0.1:1316 \
  -clients 20000 \
  -duration 240s \
  -rate-per-client 0.1 \
  -rate-schedule poisson \
  -request-timeout 30s \
  -drain 30s \
  -payload 256 \
  -inflight 1 \
  -warmup 5 \
  -receiver-mode random-online \
  -connect-ramp 45s \
  -scenario im_scaling_20000c_0_1rps_random_ramp45s \
  -out benchmark-results/02-scaling-im-20000c
```

Use this process:

1. Read `summary.json`.
2. Extract failed rows from `latency.csv`.
3. Convert failed request send and timeout timestamps to wall-clock time.
4. Align those timestamps with `top.log`, `vmstat.log`, and `ss-summary.log`.
5. Check whether established connections stayed stable until measurement ended.
6. Check whether failures cluster near the measurement end.

If failures happen near teardown, use the benchmark drain period:

```text
-drain 30s
```

The benchmark stops sending measured messages, keeps connections open, and continues reading remaining push/ACK frames
before closing clients. This avoids random receivers disappearing while senders still wait for ACKs, and reduces mass
connection-close events competing with late ACK delivery.

## Server-Side Root Causes To Investigate

If request timeouts remain after benchmark lifecycle fixes, investigate the server hot path.

### Event Loop Blocking

Risk:

- Reactor or epoll thread performs DB writes, blocking locks, or heavy business logic.

Fix direction:

- Keep network IO threads responsible for read/write and dispatch only.
- Move business work and persistence to worker pools.
- Ensure slow storage operations cannot block all connections.

### Message Persistence And Dedup Long Tail

Risk:

- ACK waits for synchronous ScyllaDB client-message dedup reads/writes.
- Occasional storage latency becomes client-observed ACK latency.
- Async message persistence can still create callback work and storage pressure, but it should not block the immediate ACK path.

Fix direction:

- Decide ACK and idempotency semantics explicitly.
- For benchmark isolation, run the server with `TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1` to remove synchronous dedup storage from the ACK hot path.
- If this eliminates timeouts, keep production dedup enabled by default and redesign dedup as a cache/async path before claiming production-equivalent numbers.
- Tune DB connection pools and request timeouts.
- Batch writes where safe.
- Add per-stage latency logs around persistence and dedup.

### Coarse Locks

Risk:

- Global online-user map, session map, connection registry, push queue, or ID generator lock causes tail latency under
  20k connections.

Fix direction:

- Use sharded maps or finer-grained locks.
- Avoid holding locks while doing IO, serialization, or DB work.
- Measure lock wait time if possible.

### Synchronous Push Writes

Risk:

- Sender ACK path synchronously writes a push to the receiver connection.
- A slow receiver or backpressured socket delays the sender ACK.

Fix direction:

- Per-connection nonblocking output queue.
- Business thread only enqueues push work.
- Event loop drains socket when writable.
- Bound queue size and define backpressure behavior.

### Teardown Storm

Risk:

- At the end of a rate-mode run, many clients stop and close at nearly the same time.
- Server handles mass close events while late ACKs are still pending.

Fix direction:

- Add benchmark drain period.
- Optionally add staggered client shutdown.
- Exclude teardown time from measurement QPS.

## Instrumentation To Add

Add slow-path logs in the P2P server path. Only log requests that exceed thresholds such as 100ms, 1s, 5s, and 30s.

Recommended stages:

```text
request read complete
protobuf parse complete
auth/session lookup complete
receiver lookup complete
message persistence start
message persistence end
push enqueue start
push enqueue end
ACK enqueue/write start
ACK enqueue/write end
```

Each slow log should include:

```text
client seq
sender user id
receiver user id
message id if available
connection id or fd
stage durations
total duration
thread id
```

This tells whether a timeout was never received by the server, delayed in business logic, delayed by storage, delayed by
push, or delayed by socket write.

## Perf And Flamegraphs

Use `perf` when CPU is high:

```bash
perf record -F 99 -g -p <server_pid> -- sleep 120
perf report
```

Or generate a flamegraph if FlameGraph tools are available:

```bash
perf script > perf.script
stackcollapse-perf.pl perf.script > perf.folded
flamegraph.pl perf.folded > perf.svg
```

Look for:

```text
protobuf serialization/deserialization
DB driver calls
mutex lock contention
session or connection map lookups
send/write path
timer processing
event-loop dispatch overhead
```

If CPU is low but timeouts occur, flamegraphs may not be enough. Focus on blocked IO, lock waits, DB latency, and
queueing.

## Local Versus Cloud Benchmarking

Local DevContainer results are useful for development, but they are not a clean production benchmark:

- Server and Go load generator compete for CPU.
- Loopback TCP doubles the visible socket endpoint count.
- Docker Desktop or LinuxKit can introduce scheduling noise.
- Other processes inside the workspace can affect CPU and memory.
- Container memory pressure can kill the load generator.

For AWS or other cloud validation:

- Use one machine for the server.
- Use one or more separate machines for load generators.
- Set high `ulimit -n`.
- Tune ephemeral port range and TCP backlog if needed.
- Pin or isolate noisy services where possible.
- Record instance type, CPU count, memory, kernel, compiler build type, and network topology.

Suggested cloud scenarios:

```text
connection capacity: 50k clients * 0.02 msg/s = about 1,000 msg/s
capacity plus throughput: 50k clients * 0.05 msg/s = about 2,500 msg/s
throughput pressure: 10k clients * 0.5 msg/s = about 5,000 msg/s
throughput pressure: 20k clients * 0.2 msg/s = about 4,000 msg/s
```

Do not jump directly to 50k high-frequency sends. Separate connection capacity from message throughput.

## Tuning History

The table below records the key benchmark/tuning steps from this investigation. Use it to distinguish benchmark-driver
fixes from real server-side optimizations.

| Step | Main Change | Scenario | Valid | Errors | Success | QPS | p50 / p95 / p99 / p999 | Interpretation |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Stable business-like baseline | Low per-user frequency, random online receivers | 20k clients, 0.05 msg/s, 240s | true | none | 249760/249760 | about 1k expected; old end-to-end output showed 533.79 before QPS denominator fix | 0.965 / 28.117 / 160.031 / 353.151 ms | Good resume-safe capacity line. |
| Higher pressure before fixes | 20k clients at 0.2 msg/s | 20k clients, 0.2 msg/s, 240s | false | request failures | 959620/959630 | old output 3224.21 before denominator fix | 22.430 / 1683.145 / 2298.244 / 2739.305 ms | Tail latency already high; not suitable as a clean capacity claim. |
| QPS denominator fixed | Measurement-window QPS plus end-to-end QPS split | 20k clients, 0.2 msg/s, 240s | false | warmup timeout | 959006/959012 | 3995.86 measurement, 3314.94 end-to-end | 3.505 / 909.120 / 1480.714 / 1880.599 ms | Throughput reporting became correct; failures remained. |
| Error classification and reconnect improvements | Non-measurement reconnect, visible error counts | 20k clients, 0.2 msg/s, 240s | false | timeout=3, unknown=15, warmup_timeout=1 | 960503/960521 | 4002.10 measurement | 3.098 / 747.499 / 1653.690 / 1998.066 ms | Error visibility improved; `unknown` still hid ACK sequencing effects. |
| Stale ACK fix candidate run | Skip late ACKs with older seq | 20k clients, 0.1 msg/s, 240s | false | timeout=3, unknown=24, warmup_timeout=1 | 480498/480525 | 2002.08 | 0.911 / 14.464 / 66.764 / 220.646 ms | Raw failures showed timeout followed by ACK sequence drift. |
| Stale ACK fix verified | `skipped_stale_acks` added, unknown cascade removed | 20k clients, 0.1 msg/s, 240s | false | timeout=3 | 480679/480682 | 2002.83 | 0.926 / 15.567 / 60.047 / 150.834 ms | Benchmark ACK sequencing issue fixed; remaining failures were real timeouts. |
| Monitored run | `top`, `vmstat`, `ss` collection | 20k clients, 0.1 msg/s, 240s | false | timeout=4, warmup_timeout=1 | 480608/480612 | 2002.53 | 1.331 / 205.271 / 880.868 / 2179.416 ms | Failures occurred with stable connections; local tail latency visible. |
| Drain added | `-drain 30s` keeps clients connected after measured send window | 20k clients, 0.1 msg/s, 240s | false | timeout=2, warmup_timeout=1 | 480794/480796 | 2003.31 | 1.526 / 515.214 / 1443.359 / 2422.530 ms | Teardown amplification reduced but did not remove server-side tail latency. |
| Sync dedup disabled for benchmark | `TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1` removes synchronous Scylla dedup from ACK hot path | 20k clients, 0.1 msg/s, 240s | true | warmup_timeout=1 only | 480475/480475 | 2001.98 | 0.225 / 3.581 / 36.485 / 129.129 ms | Official measured window became clean; synchronous dedup was the dominant tail source. |
| Local overload attempt | Increased client count and throughput too far for local driver | 30k clients, 0.2 msg/s, 240s | no result | process killed | none | none | none | Go load generator/container likely hit resource limits; not a server capacity result. |

Notes:

- `warmup_timeout=1` is a pre-measurement recovery event. It should be investigated, but it is separate from official
  measured request failures when `summary.valid` is true.
- QPS values after the denominator fix should use measurement-window `Success QPS`, not end-to-end QPS.
- The optimized 2,000 msg/s result is valid for the ACK hot path with synchronous dedup disabled. It should not be
  described as production-equivalent idempotency unless a replacement dedup design is enabled.

## Tuning Validation Result

After adding `-drain 30s`, the `20k * 0.1 msg/s` run still had rare official measurement timeouts. The remaining
failures happened while the connection count was stable, and no stale ACKs were observed during drain. This pointed away
from benchmark ACK sequencing and teardown timing, and toward a real server-side ACK hot-path tail latency issue.

The P2P send path was then inspected. `MsgService::send_p2p_message` performed synchronous ScyllaDB client-message
dedup reads and writes before returning the immediate ACK. Because the benchmark sends a unique `client_msg_id` for every
message and does not retry, this added storage reads/writes to every ACK in the measured hot path.

A benchmark-only switch was added and should be enabled for the current optimized benchmark pass:

```bash
TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1
```

Reliability impact:

- It does not disable async message persistence; message bodies are still enqueued to `AsyncMsgWriter`.
- It disables the synchronous `client_msg_id` dedup table read/write before the immediate ACK.
- Duplicate client retries can produce duplicate messages in this mode unless handled elsewhere.
- Production should keep synchronous dedup enabled by default, or replace it with a fast cache/asynchronous dedup design.

With the server started as:

```bash
TERMCHAT_IDLE_TIMEOUT_MS=300000 TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1 ./build/relwithdebinfo/server/src/server -l 0
```

the 20,000-client, 0.1 msg/s, random-online, Poisson benchmark produced:

```text
valid: true
clients: 20000
duration: 240s
rate_per_client: 0.1 msg/s
receiver_mode: random-online
request_timeout: 30s
drain: 30s
success: 480475/480475
failed: 0
attempted_qps: 2001.98
success_qps: 2001.98
p50/p95/p99/p999: 0.225 / 3.581 / 36.485 / 129.129 ms
max: 10575.868 ms
skipped_stale_acks: 0
errors: warmup_timeout=1
```

`warmup_timeout=1` was a pre-measurement recovery event. There were no failed rows in `latency.csv`, and the official
measurement result was `valid: true`.

Conclusion:

- The benchmark client lifecycle and ACK sequencing are now sufficiently correct for this scenario.
- The previous official measurement timeouts were primarily caused by synchronous ScyllaDB client-message dedup on the
  immediate ACK hot path.
- Removing that synchronous storage work reduced p99 latency from hundreds of milliseconds/seconds-level tails to about
  36ms in this local run, and eliminated official measurement failures at about 2,000 msg/s.
- This result is a valid benchmark of the optimized ACK hot path, but it is not identical to production idempotency
  semantics unless duplicate client messages are handled by another fast path.

Production follow-up:

- Keep synchronous dedup enabled by default for correctness.
- Replace the synchronous storage dedup path with an in-memory or sharded short-TTL dedup cache backed by asynchronous
  persistence.
- Keep Scylla dedup writes asynchronous or off the immediate ACK path.
- Add slow-stage logs for dedup, push enqueue, and ACK enqueue to verify this remains true under cloud benchmarks.

## Resume Guidance

Use one stable performance line, not the largest invalid pressure result.

Best current stable production-semantics style:

```text
Built and benchmarked a C++20 epoll/Reactor IM server, sustaining 20,000 long-lived TCP connections and about
1,000 random private messages/s with 100% success and p99 latency around 160ms in a local DevContainer benchmark.
```

Optimized ACK hot-path style, if the context allows explaining the benchmark switch:

```text
Optimized the P2P ACK hot path by removing synchronous ScyllaDB dedup from the measured path, improving a 20,000
connection random-message benchmark to about 2,000 msg/s with 100% measured success and p99 latency around 36ms.
```

Be precise: the 2,000 msg/s number is valid for the optimized ACK hot path with `TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP=1`.
For production-equivalent semantics, either keep the 1,000 msg/s stable line or implement an asynchronous/cache-backed
dedup design and rerun the benchmark.

## Practical Next Steps

1. Implement production-grade fast dedup: short-TTL in-memory/sharded cache plus asynchronous Scylla persistence.
2. Rerun `20k * 0.1 msg/s` with production-equivalent dedup enabled.
3. Try `20k * 0.2 msg/s` only after the 2,000 msg/s run is stable without benchmark-only shortcuts.
4. Add server-side P2P stage timing logs before further tuning.
5. If CPU is high during failures, capture `perf` and flamegraph data.
6. If CPU is low during failures, inspect lock waits, DB tail latency, and output queue behavior.
7. Keep the resume number on the latest `valid: true` scenario whose semantics you can explain clearly.
