#!/usr/bin/env bash
set -euo pipefail

SCYLLA_HOST=${SCYLLA_HOST:-scylla}
SCYLLA_PORT=${SCYLLA_PORT:-9042}
DOCKER_COMPOSE=(docker compose -f .devcontainer/docker-compose.yml)

echo "[scylla] waiting for ScyllaDB..."
until "${DOCKER_COMPOSE[@]}" exec -T scylla cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" -e "DESCRIBE KEYSPACES" > /dev/null 2>&1; do
  sleep 2
done

echo "[scylla] initializing keyspace and tables..."
"${DOCKER_COMPOSE[@]}" exec -T scylla cqlsh "$SCYLLA_HOST" "$SCYLLA_PORT" <<'CQL'
CREATE KEYSPACE IF NOT EXISTS im
WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};

CREATE TABLE IF NOT EXISTS im.messages (
  conversation_id text,
  message_id bigint,
  timestamp bigint,
  sender_id bigint,
  receiver_id bigint,
  content_type int,
  content blob,
  PRIMARY KEY (conversation_id, message_id)
);

CREATE TABLE IF NOT EXISTS im.user_messages_by_id (
  user_id bigint,
  message_id bigint,
  sender_id bigint,
  receiver_id bigint,
  content_type int,
  content blob,
  timestamp bigint,
  PRIMARY KEY (user_id, message_id)
) WITH CLUSTERING ORDER BY (message_id ASC);
CQL

echo "[scylla] init done."
