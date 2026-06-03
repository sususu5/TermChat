#!/usr/bin/env bash
set -euo pipefail

PERF_DATA="${PERF_DATA:-perf.data}"
OUT="${OUT:-perf.svg}"

find_perf() {
    if [[ -n "${PERF_BIN:-}" ]]; then
        printf '%s\n' "${PERF_BIN}"
        return
    fi

    local candidate
    for candidate in /usr/lib/linux-tools*/perf /usr/bin/perf; do
        if [[ -x "${candidate}" ]]; then
            if "${candidate}" --version >/dev/null 2>&1; then
                printf '%s\n' "${candidate}"
                return
            fi
        fi
    done
}

PERF="$(find_perf)"
if [[ -z "${PERF}" ]]; then
    echo "ERROR: usable perf binary not found." >&2
    echo "Install linux-tools/perf for this environment, or set PERF_BIN=/path/to/perf." >&2
    exit 1
fi

if [[ ! -s "${PERF_DATA}" ]]; then
    echo "ERROR: ${PERF_DATA} is missing or empty." >&2
    echo "Capture samples first, for example:" >&2
    echo "  ${PERF} record -F 99 -p \$(pgrep server) -g -- sleep 120" >&2
    exit 1
fi

if ! command -v stackcollapse-perf.pl >/dev/null 2>&1; then
    echo "ERROR: stackcollapse-perf.pl not found in PATH." >&2
    exit 1
fi
if ! command -v flamegraph.pl >/dev/null 2>&1; then
    echo "ERROR: flamegraph.pl not found in PATH." >&2
    exit 1
fi

TMP_SCRIPT="$(mktemp)"
TMP_FOLDED="$(mktemp)"
trap 'rm -f "${TMP_SCRIPT}" "${TMP_FOLDED}"' EXIT

LC_ALL=C LANG=C "${PERF}" script -i "${PERF_DATA}" >"${TMP_SCRIPT}"
LC_ALL=C LANG=C stackcollapse-perf.pl "${TMP_SCRIPT}" >"${TMP_FOLDED}"

if [[ ! -s "${TMP_FOLDED}" ]]; then
    echo "ERROR: no stack samples found in ${PERF_DATA}." >&2
    echo "The capture may have failed, used the wrong perf binary, or recorded while the process was idle/exited." >&2
    exit 1
fi

LC_ALL=C LANG=C flamegraph.pl "${TMP_FOLDED}" >"${OUT}"
echo "Wrote ${OUT} using ${PERF}"
