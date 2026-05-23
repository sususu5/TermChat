# Message Reliability Model

TermChat separates message reliability into storage and delivery dimensions. `PERSISTED` and `DELIVERED` are independent facts: either one may arrive first because ScyllaDB persistence and online push confirmation run on different paths.

## Reliability Flow

```mermaid
stateDiagram-v2
    [*] --> ClientSent: send
    ClientSent --> ServerReceived: accept
    ServerReceived --> Persisted: persist
    ServerReceived --> PushEnqueued: enqueue
    PushEnqueued --> ClientDelivered: push
    Persisted --> SenderNotified: ACK PERSISTED
    ClientDelivered --> SenderNotified: ACK DELIVERED
    ClientSent --> ClientSent: retry
    ServerReceived --> RetryDeduped: dedup
```

The diagram shows the main flow, not a strict total order. After `ServerReceived`, persistence and push can complete in either order.

## States

| Dimension | State | Meaning | Main owner |
| --- | --- | --- | --- |
| Client | `client_sent` | The client has sent the request and keeps it in the pending queue until an ACK or retry limit is reached. | Client |
| Server accept | `server_received` | The server has authenticated the sender, accepted the request, generated the authoritative `msg_id`, and recorded the dedup key. | Server service |
| Storage | `persisted` | The async writer has flushed the message into ScyllaDB message tables. | Server DAO / async writer |
| Delivery | `push_enqueued` | The server has placed the push message into the receiver connection's outgoing queue. | Push service |
| Delivery | `client_delivered` | The receiver client has received `CMD_P2P_MSG_PUSH` and returned `MessageAck(DELIVERED)`. | Receiver client |

## ACK Merge Rule

ACKs are handled as independent state updates instead of overwriting a single status enum. This prevents state regression when ACKs arrive out of order.

```text
ACK_STATUS_RECEIVED  -> server_received = true
ACK_STATUS_PERSISTED -> server_received = true, persisted = true
ACK_STATUS_ENQUEUED  -> server_received = true, push_enqueued = true
ACK_STATUS_DELIVERED -> server_received = true, push_enqueued = true, delivered = true
```

## Scenario Analysis

- **Normal online delivery**: the sender receives an initial `ENQUEUED` ACK after the server accepts and queues the push, then receives async `PERSISTED` and `DELIVERED` ACKs as storage and receiver confirmation complete.
- **Receiver offline**: the message is still persisted in ScyllaDB. When the receiver reconnects, the client sends `last_ack_msg_id` and pulls only incremental messages instead of loading a fixed-size recent window.
- **Sender retry after timeout**: the client retransmits the same `client_msg_id`. The server uses the dedup table to return the original `msg_id` and current status instead of creating duplicate messages.
- **Push succeeds but persistence is slower**: online delivery and durable storage are decoupled. The state machine exposes both stages, which helps diagnose whether latency comes from network push or ScyllaDB writes.
- **Persistence succeeds before push ACK**: the sender can observe `PERSISTED` before `DELIVERED`. The client merges both facts without assuming that later ACKs are more advanced.
- **Read receipts intentionally omitted**: the system focuses on reliable delivery and synchronization. `READ` is kept as an extensible protocol state, but the current implementation does not require UI-level read tracking.
