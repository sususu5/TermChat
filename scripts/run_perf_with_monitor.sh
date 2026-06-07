#!/usr/bin/env bash
set -euo pipefail

PERF_CONFIG="${PERF_CONFIG:-}"
if [[ -n "${PERF_CONFIG}" ]]; then
    # shellcheck source=/dev/null
    source "${PERF_CONFIG}"
elif [[ -f scripts/perf.env ]]; then
    # shellcheck source=/dev/null
    source scripts/perf.env
fi

BENCH_CLIENTS="${BENCH_CLIENTS:-25000}"
BENCH_DURATION="${BENCH_DURATION:-240s}"
BENCH_RATE_PER_CLIENT="${BENCH_RATE_PER_CLIENT:-0.1}"
BENCH_RATE_SCHEDULE="${BENCH_RATE_SCHEDULE:-poisson}"
BENCH_REQUEST_TIMEOUT="${BENCH_REQUEST_TIMEOUT:-30s}"
BENCH_DRAIN="${BENCH_DRAIN:-30s}"
BENCH_PAYLOAD="${BENCH_PAYLOAD:-256}"
BENCH_INFLIGHT="${BENCH_INFLIGHT:-1}"
BENCH_WARMUP="${BENCH_WARMUP:-5}"
BENCH_RECEIVER_MODE="${BENCH_RECEIVER_MODE:-random-online}"
BENCH_CONNECT_RAMP="${BENCH_CONNECT_RAMP:-45s}"
BENCH_RATE_LABEL="${BENCH_RATE_PER_CLIENT//./_}"
BENCH_SCENARIO="${BENCH_SCENARIO:-im_scaling_${BENCH_CLIENTS}c_${BENCH_RATE_LABEL}rps_random_ramp${BENCH_CONNECT_RAMP%s}}"

OUT_DIR="${OUT_DIR:-benchmark-results/${BENCH_SCENARIO}}"
MONITOR_DIR="${MONITOR_DIR:-${OUT_DIR}/monitor}"
INTERVAL="${MONITOR_INTERVAL:-1}"
BENCH_ADDR="${BENCH_ADDR:-127.0.0.1:1316}"
BENCH_HOST="${BENCH_ADDR%:*}"
BENCH_PORT="${BENCH_ADDR##*:}"
BENCH_PORT_HEX="$(printf '%04X' "${BENCH_PORT}")"
SERVER_AUTOSTART="${SERVER_AUTOSTART:-1}"
SERVER_ARGS="${SERVER_ARGS:--l 0}"
SERVER_LOG="${MONITOR_DIR}/server.log"

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

port_is_open() {
    timeout 1 bash -c ":</dev/tcp/${BENCH_HOST}/${BENCH_PORT}" >/dev/null 2>&1
}

find_server_bin() {
    if [[ -n "${SERVER_BIN:-}" ]]; then
        printf '%s\n' "${SERVER_BIN}"
        return
    fi

    local candidate
    for candidate in \
        ./build/relwithdebinfo/server/src/server \
        ./build/server-release/server/src/server \
        ./build/debug/server/src/server; do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return
        fi
    done
}

ensure_server() {
    if port_is_open; then
        echo "server_listening=already" >>"${META_LOG}"
        return
    fi

    if [[ "${SERVER_AUTOSTART}" != "1" ]]; then
        echo "server_listening=false" >>"${META_LOG}"
        echo "ERROR: ${BENCH_ADDR} is not listening. Start the server first or set SERVER_AUTOSTART=1." | tee "${BENCH_LOG}"
        exit 1
    fi

    local server_bin
    server_bin="$(find_server_bin)"
    if [[ -z "${server_bin}" ]]; then
        echo "server_listening=false" >>"${META_LOG}"
        echo "ERROR: no server binary found. Build one of: build/relwithdebinfo, build/server-release, build/debug." | tee "${BENCH_LOG}"
        exit 1
    fi

    echo "server_autostart_bin=${server_bin}" >>"${META_LOG}"
    echo "server_autostart_args=${SERVER_ARGS}" >>"${META_LOG}"
    echo "Starting server: ${server_bin} ${SERVER_ARGS}" | tee "${SERVER_LOG}"
    TERMCHAT_IDLE_TIMEOUT_MS="${TERMCHAT_IDLE_TIMEOUT_MS:-300000}" \
    TERMCHAT_FAST_CLIENT_DEDUP="${TERMCHAT_FAST_CLIENT_DEDUP:-1}" \
        "${server_bin}" ${SERVER_ARGS} >>"${SERVER_LOG}" 2>&1 &
    PIDS+=("$!")

    for _ in {1..60}; do
        if port_is_open; then
            echo "server_listening=autostarted" >>"${META_LOG}"
            return
        fi
        sleep 1
    done

    echo "server_listening=false" >>"${META_LOG}"
    echo "ERROR: server did not start listening on ${BENCH_ADDR}. See ${SERVER_LOG}." | tee "${BENCH_LOG}"
    exit 1
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
    echo "perf_config=${PERF_CONFIG:-scripts/perf.env if present}"
    echo "bench_addr=${BENCH_ADDR}"
    echo "bench_clients=${BENCH_CLIENTS}"
    echo "bench_duration=${BENCH_DURATION}"
    echo "bench_rate_per_client=${BENCH_RATE_PER_CLIENT}"
    echo "bench_rate_schedule=${BENCH_RATE_SCHEDULE}"
    echo "bench_connect_ramp=${BENCH_CONNECT_RAMP}"
    echo "bench_request_timeout=${BENCH_REQUEST_TIMEOUT}"
    echo "bench_drain=${BENCH_DRAIN}"
    echo "bench_payload=${BENCH_PAYLOAD}"
    echo "bench_inflight=${BENCH_INFLIGHT}"
    echo "bench_warmup=${BENCH_WARMUP}"
    echo "bench_receiver_mode=${BENCH_RECEIVER_MODE}"
    echo "bench_scenario=${BENCH_SCENARIO}"
    echo "uname=$(uname -a)"
    echo "ulimit_n=$(ulimit -n)"
} >"${META_LOG}"

ensure_server

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
            ss -H -tan state established "( sport = :${BENCH_PORT} or dport = :${BENCH_PORT} )" | wc -l
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
                NR > 1 && $4 == "01" && (endpoint_port($2) == port || endpoint_port($3) == port) {
                    count++
                }
                END {
                    print count + 0
                }
            ' port="${BENCH_PORT_HEX}" /proc/net/tcp /proc/net/tcp6 2>/dev/null
            sleep "${INTERVAL}"
        done
    ) >"${SS_ESTABLISHED_LOG}" &
    PIDS+=("$!")
fi

go run ./tests/perf \
    -addr "${BENCH_ADDR}" \
    -clients "${BENCH_CLIENTS}" \
    -duration "${BENCH_DURATION}" \
    -rate-per-client "${BENCH_RATE_PER_CLIENT}" \
    -rate-schedule "${BENCH_RATE_SCHEDULE}" \
    -request-timeout "${BENCH_REQUEST_TIMEOUT}" \
    -drain "${BENCH_DRAIN}" \
    -payload "${BENCH_PAYLOAD}" \
    -inflight "${BENCH_INFLIGHT}" \
    -warmup "${BENCH_WARMUP}" \
    -receiver-mode "${BENCH_RECEIVER_MODE}" \
    -connect-ramp "${BENCH_CONNECT_RAMP}" \
    -scenario "${BENCH_SCENARIO}" \
    -out "${OUT_DIR}" 2>&1 | tee "${BENCH_LOG}"

{
    echo "finished_at=$(date --iso-8601=seconds)"
} >>"${META_LOG}"
