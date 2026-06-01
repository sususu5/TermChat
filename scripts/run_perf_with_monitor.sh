#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${OUT_DIR:-benchmark-results/02-scaling-im-25000c}"
MONITOR_DIR="${MONITOR_DIR:-${OUT_DIR}/monitor}"
INTERVAL="${MONITOR_INTERVAL:-1}"

mkdir -p "${MONITOR_DIR}"

BENCH_LOG="${MONITOR_DIR}/bench.log"
TOP_LOG="${MONITOR_DIR}/top.log"
VMSTAT_LOG="${MONITOR_DIR}/vmstat.log"
SS_SUMMARY_LOG="${MONITOR_DIR}/ss-summary.log"
SS_ESTABLISHED_LOG="${MONITOR_DIR}/ss-established.log"
META_LOG="${MONITOR_DIR}/meta.log"

PIDS=()

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

cleanup() {
    local status=$?
    for pid in "${PIDS[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
    done
    wait "${PIDS[@]:-}" 2>/dev/null || true
    exit "${status}"
}
trap cleanup EXIT INT TERM

{
    echo "started_at=$(date --iso-8601=seconds)"
    echo "out_dir=${OUT_DIR}"
    echo "monitor_dir=${MONITOR_DIR}"
    echo "interval_seconds=${INTERVAL}"
    echo "uname=$(uname -a)"
    echo "ulimit_n=$(ulimit -n)"
} >"${META_LOG}"

if command_exists top; then
    top -b -d "${INTERVAL}" >"${TOP_LOG}" &
    PIDS+=("$!")
else
    echo "top command not found" >"${TOP_LOG}"
fi

if command_exists vmstat; then
    vmstat "${INTERVAL}" >"${VMSTAT_LOG}" &
    PIDS+=("$!")
else
    echo "vmstat command not found" >"${VMSTAT_LOG}"
fi

if command_exists ss; then
    (
        while true; do
            date --iso-8601=seconds
            ss -s
            sleep "${INTERVAL}"
        done
    ) >"${SS_SUMMARY_LOG}" &
    PIDS+=("$!")

    (
        while true; do
            date --iso-8601=seconds
            ss -H -tan state established '( sport = :1316 or dport = :1316 )' | wc -l
            sleep "${INTERVAL}"
        done
    ) >"${SS_ESTABLISHED_LOG}" &
    PIDS+=("$!")
else
    (
        echo "ss command not found; using /proc/net/sockstat fallback"
        while true; do
            date --iso-8601=seconds
            cat /proc/net/sockstat
            if [[ -r /proc/net/sockstat6 ]]; then
                cat /proc/net/sockstat6
            fi
            sleep "${INTERVAL}"
        done
    ) >"${SS_SUMMARY_LOG}" &
    PIDS+=("$!")

    (
        echo "ss command not found; counting established TCP sockets for port 1316 from /proc/net/tcp*"
        while true; do
            date --iso-8601=seconds
            awk '
                function endpoint_port(endpoint, parts) {
                    split(endpoint, parts, ":")
                    return parts[2]
                }
                NR > 1 && $4 == "01" && (endpoint_port($2) == "0524" || endpoint_port($3) == "0524") {
                    count++
                }
                END {
                    print count + 0
                }
            ' /proc/net/tcp /proc/net/tcp6 2>/dev/null
            sleep "${INTERVAL}"
        done
    ) >"${SS_ESTABLISHED_LOG}" &
    PIDS+=("$!")
fi

go run ./tests/perf \
    -addr 127.0.0.1:1316 \
    -clients 25000 \
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
    -scenario im_scaling_25000c_0_1rps_random_ramp45s \
    -out "${OUT_DIR}" 2>&1 | tee "${BENCH_LOG}"

{
    echo "finished_at=$(date --iso-8601=seconds)"
} >>"${META_LOG}"
