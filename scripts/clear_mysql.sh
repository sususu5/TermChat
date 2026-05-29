#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

COMPOSE_FILE="${COMPOSE_FILE:-$PROJECT_ROOT/.devcontainer/docker-compose.yml}"
MYSQL_SERVICE="${MYSQL_SERVICE:-mysql}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-123456}"
MYSQL_DATABASE="${MYSQL_DATABASE:-testdb}"
SCYLLA_SERVICE="${SCYLLA_SERVICE:-scylla}"
SCYLLA_HOST="${SCYLLA_HOST:-scylla}"
SCYLLA_PORT="${SCYLLA_PORT:-9042}"

echo "[mysql] clearing ${MYSQL_DATABASE}.im_friend and ${MYSQL_DATABASE}.im_user..."

docker compose -f "$COMPOSE_FILE" exec -T "$MYSQL_SERVICE" \
    mysql -u "$MYSQL_USER" "-p${MYSQL_PASSWORD}" "$MYSQL_DATABASE" \
    -e "SET FOREIGN_KEY_CHECKS=0; TRUNCATE TABLE im_friend; TRUNCATE TABLE im_user; SET FOREIGN_KEY_CHECKS=1;"

echo "[scylla] clearing im.messages..."
docker compose -f "$COMPOSE_FILE" exec -T "$SCYLLA_SERVICE" \
    cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" -e "TRUNCATE im.messages"

echo "[scylla] clearing im.user_messages_by_id..."
docker compose -f "$COMPOSE_FILE" exec -T "$SCYLLA_SERVICE" \
    cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" -e "TRUNCATE im.user_messages_by_id"

echo "[scylla] clearing im.client_msg_dedup..."
docker compose -f "$COMPOSE_FILE" exec -T "$SCYLLA_SERVICE" \
    cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" -e "TRUNCATE im.client_msg_dedup"

mysql_count() {
    local table="$1"
    docker compose -f "$COMPOSE_FILE" exec -T "$MYSQL_SERVICE" \
        mysql -N -B -u "$MYSQL_USER" "-p${MYSQL_PASSWORD}" "$MYSQL_DATABASE" \
        -e "SELECT COUNT(*) FROM ${table};"
}

scylla_count() {
    local table="$1"
    docker compose -f "$COMPOSE_FILE" exec -T "$SCYLLA_SERVICE" \
        cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" -e "SELECT COUNT(*) FROM ${table};" |
        awk '/^[[:space:]]*[0-9]+[[:space:]]*$/ { gsub(/[[:space:]]/, "", $0); print; exit }'
}

assert_zero() {
    local name="$1"
    local count="$2"
    if [ "$count" != "0" ]; then
        echo "[verify] ${name}: expected 0, got ${count}" >&2
        exit 1
    fi
    echo "[verify] ${name}: ${count}"
}

echo "[verify] checking row counts..."
assert_zero "mysql.im_friend" "$(mysql_count im_friend)"
assert_zero "mysql.im_user" "$(mysql_count im_user)"
assert_zero "scylla.im.messages" "$(scylla_count im.messages)"
assert_zero "scylla.im.user_messages_by_id" "$(scylla_count im.user_messages_by_id)"
assert_zero "scylla.im.client_msg_dedup" "$(scylla_count im.client_msg_dedup)"

echo "[clear] all database tables are empty."
