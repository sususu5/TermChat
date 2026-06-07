# AWS Scaling Benchmark Runbook

This runbook describes a reproducible two-host AWS benchmark setup for the IM scaling test.

## Target Environment

Use two EC2 instances in the same Region, VPC, subnet/AZ if possible:

| Role | AMI | Architecture | Instance type | Root volume |
| --- | --- | --- | --- | --- |
| Server | Ubuntu Server 24.04 LTS | arm64 | c7g.2xlarge | 80 GiB gp3 |
| Benchmark client | Ubuntu Server 24.04 LTS | arm64 | c7g.2xlarge | 80 GiB gp3 |

Security group rules:

- Allow SSH `22/tcp` from your workstation.
- Allow TermChat `1316/tcp` from the benchmark client private IP to the server.
- Keep MySQL `3306/tcp` and Scylla `9042/tcp` private to the server host unless you intentionally split database hosts.

Use the server private IP in benchmark commands. Avoid sending benchmark traffic through public IP/NAT.

## 1. Prepare Both Hosts

Clone the repository on both instances, then bootstrap the build dependencies:

```bash
git clone <repo-url> TermChat
cd TermChat
./scripts/bootstrap_ec2_build_deps.sh
source ~/.termchat-build-env
```

Apply runtime limits on both hosts:

```bash
sudo tee /etc/sysctl.d/99-termchat-benchmark.conf >/dev/null <<'EOF'
fs.file-max = 1048576
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 10000 65535
net.ipv4.tcp_tw_reuse = 1
EOF
sudo sysctl --system

sudo tee /etc/security/limits.d/99-termchat.conf >/dev/null <<'EOF'
* soft nofile 1048576
* hard nofile 1048576
ubuntu soft nofile 1048576
ubuntu hard nofile 1048576
EOF
```

Log out and back in, then verify:

```bash
ulimit -n
sysctl net.ipv4.ip_local_port_range
```

The client host needs the widened ephemeral port range for 50k outbound TCP connections to one server address.

## 2. Prepare Server Host

Install Docker for the local MySQL and Scylla services:

```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose-v2
sudo usermod -aG docker "$USER"
newgrp docker
```

Start only the database services:

```bash
docker compose up -d mysql scylla
docker compose ps
```

Copy the generated MySQL CA certificate to the path expected by the C++ server:

```bash
sudo mkdir -p /etc/mysql/certs
docker compose cp mysql:/certs/ca.pem /tmp/termchat-mysql-ca.pem
sudo cp /tmp/termchat-mysql-ca.pem /etc/mysql/certs/ca.pem
sudo chmod 644 /etc/mysql/certs/ca.pem
```

Build the server profiling binary:

```bash
cmake --preset server-perf
cmake --build --preset server-perf
```

Run the server:

```bash
MYSQL_HOST=127.0.0.1 \
MYSQL_PORT=3306 \
MYSQL_USER=root \
MYSQL_PASSWORD=123456 \
MYSQL_DATABASE=testdb \
SCYLLA_HOST=127.0.0.1 \
SCYLLA_PORT=9042 \
TERMCHAT_IDLE_TIMEOUT_MS=300000 \
TERMCHAT_FAST_CLIENT_DEDUP=1 \
./build/relwithdebinfo/server/src/server -l 0
```

Run this in `tmux` or `screen` so the server stays up if the SSH session disconnects.

Confirm the server is listening:

```bash
ss -ltnp | grep ':1316'
```

## 3. Prepare Benchmark Client Host

Build the benchmark protobuf outputs. The Go benchmark imports generated files from `build/relwithdebinfo/proto/go`, so use the dedicated benchmark-client preset:

```bash
cmake --preset benchmark-client
cmake --build --preset benchmark-client
go mod download
```

Create a client-side env file. Replace `SERVER_PRIVATE_IP` with the server instance private IP:

```bash
SERVER_PRIVATE_IP=10.0.0.10
cp scripts/perf-50k.env scripts/perf-50k-aws.env
sed -i "s/^BENCH_ADDR=.*/BENCH_ADDR=${SERVER_PRIVATE_IP}:1316/" scripts/perf-50k-aws.env
```

Run the 50k-client benchmark without autostarting a local server:

```bash
SERVER_AUTOSTART=0 PERF_CONFIG=scripts/perf-50k-aws.env scripts/run_perf_with_monitor.sh
```

The default 50k profile is:

- `BENCH_CLIENTS=50000`
- `BENCH_DURATION=240s`
- `BENCH_RATE_PER_CLIENT=0.1`
- `BENCH_RATE_SCHEDULE=poisson`
- `BENCH_CONNECT_RAMP=45s`
- `BENCH_REQUEST_TIMEOUT=30s`
- `BENCH_DRAIN=30s`
- `BENCH_PAYLOAD=256`
- `BENCH_INFLIGHT=1`
- `BENCH_WARMUP=5`
- `BENCH_RECEIVER_MODE=random-online`

## 4. Collect Results

On the client host, benchmark output is under:

```bash
benchmark-results/im_scaling_50000c_0_1rps_random_ramp45s/
```

Important files:

- `summary.json`: validity, QPS, latency percentiles, error counts.
- `latency.csv`: per-message latency records.
- `monitor/meta.log`: effective benchmark parameters.
- `monitor/bench.log`: full benchmark stdout/stderr.
- `monitor/top.log`, `monitor/vmstat.log`, `monitor/ss-summary.log`, `monitor/ss-established.log`: client-side resource and socket telemetry.

On the server host, also capture:

```bash
docker compose ps
docker compose logs mysql --tail=100
docker compose logs scylla --tail=100
ss -s
top -b -n 1
vmstat 1 5
```

Only use runs with `valid: true` and empty `errors` in `summary.json` for capacity conclusions. Compare:

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

## 5. Rerun Checklist

Before each rerun:

```bash
# Server host
docker compose ps
ss -ltnp | grep ':1316'

# Client host
ulimit -n
sysctl net.ipv4.ip_local_port_range
```

If you need a clean database between runs, destroy and recreate the database
containers and volumes on the server host, then start the database services again:

```bash
docker compose stop mysql scylla
docker compose rm -f mysql scylla
docker volume rm termchat_mysql-data termchat_ssl-certs 2>/dev/null || true

docker compose up -d mysql scylla
docker compose ps
```

After recreating the containers, copy the MySQL CA certificate again and
reinitialize the Scylla schema:

```bash
sudo mkdir -p /etc/mysql/certs
docker compose cp mysql:/certs/ca.pem /tmp/termchat-mysql-ca.pem
sudo cp /tmp/termchat-mysql-ca.pem /etc/mysql/certs/ca.pem
sudo chmod 644 /etc/mysql/certs/ca.pem
```
