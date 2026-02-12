# event/bus@v1 — ZCL1 Protocol

This document is the normative wire protocol for the golden capability `event/bus@v1`.

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described in RFC 2119.

## Identity

- kind: `"event"`
- name: `"bus"`
- version: `1`

The capability is opened via `zi_cap_open()` and yields a bidirectional stream handle.
Requests are written as ZCL1 frames; responses and asynchronous events are read back as ZCL1 frames.

## Framing

All frames use ZCL1 (see `ZCL1_PROTOCOL.md`).

- Requests MUST set `status=0`.
- Successful responses MUST set `status=1`.
- Error responses MUST set `status=0` and use the standard ZCL1 error payload format.

## Operations

ZCL1 `op` values are:

- `1` `SUBSCRIBE`
- `2` `UNSUBSCRIBE`
- `3` `PUBLISH`

Asynchronous event frames emitted by the bus use:

- `100` `EVENT`

### Common types

All integer fields are little-endian.

- `u32`: 32-bit unsigned integer

### SUBSCRIBE (op=1)

Subscribe the caller’s handle to a topic.

Request payload:

- `u32 topic_len`
- `bytes[topic_len] topic` (UTF-8 recommended; opaque bytes allowed)
- `u32 flags` (reserved; MUST be `0`)

Response payload:

- `u32 subscription_id`

Errors:

- The implementation MUST return a ZCL1 error frame on invalid payloads.

### UNSUBSCRIBE (op=2)

Remove a subscription by id.

Request payload:

- `u32 subscription_id`

Response payload:

- `u32 removed` (0 or 1)

### PUBLISH (op=3)

Publish a message to a topic.

Request payload:

- `u32 topic_len`
- `bytes[topic_len] topic`
- `u32 data_len`
- `bytes[data_len] data`

Response payload:

- `u32 delivered` (number of subscriptions that were queued an `EVENT`)

Notes:

- Publishing to a topic with no current subscribers MUST succeed and return `delivered=0`.
- Implementations MAY deliver to the publishing handle if it is subscribed.

## EVENT (op=100)

An asynchronous event emitted to subscribers.

Payload:

- `u32 subscription_id`
- `u32 topic_len`
- `bytes[topic_len] topic`
- `u32 data_len`
- `bytes[data_len] data`

Events MUST be emitted as ZCL1 ok frames (`status=1`). The event frame’s `rid` MUST match the request `rid` of the `PUBLISH` that caused it.

## Backpressure and ordering

- Within a single handle, frames returned by `zi_read` MUST preserve the order they were queued by the runtime.
- Implementations MAY apply backpressure if output buffering is exhausted.
  - If the runtime cannot enqueue an event due to buffering constraints, it MAY omit delivering that event to that subscriber.
  - In this case, the `delivered` count in the `PUBLISH` response MUST reflect only the successfully queued deliveries.

## Threading model

`event/bus@v1` is scheduler-agnostic and does not define threads, fibers, or an event loop.
It is purely a message delivery protocol over a zABI handle.
