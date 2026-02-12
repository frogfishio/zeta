# sys/loop@v1 — ZCL1 Protocol

This document is the normative wire protocol for the capability `sys/loop@v1`.

It provides a **single, central wait primitive** for high-concurrency guests (fibers/promises/event loops):
- readiness notifications for registered handles (readable/writable/hup/error)
- timers (one-shot and repeating)

The intent is that guests **never block inside other capabilities**. Instead, other caps return `ZI_E_AGAIN` for would-block situations, and the guest blocks only by calling `POLL` on `sys/loop`.

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described in RFC 2119.

## Identity

- Kind: `sys`
- Name: `loop`
- Version: `1`

Open parameters: none.

## Transport

The handle is a ZCL1 request/response stream.

- Guest sends ZCL1 request frames via `zi_write(handle, ...)`.
- Host returns ZCL1 response frames readable via `zi_read(handle, ...)`.

ZCL1 frames:
- `version` MUST be `1`.
- Responses use ZCL1 `status` field:
  - `status=1` for OK responses.
  - `status=0` for error responses (with ZCL1 error payload).

## Overview

A guest typically:

1. Performs nonblocking work on other handles (e.g. `net/tcp`, `net/http`), handling `ZI_E_AGAIN`.
2. Registers interest in readiness via `WATCH`.
3. Calls `POLL` to block until at least one event is available (or timeout).
4. Uses returned events to resume fibers/promises.

This cap is intentionally **scheduler-agnostic**: it provides events; the guest decides how to schedule fibers.

## Integration Pattern (Recommended)

This section is informative but reflects the intended usage model.

### Guest-side rule of thumb

For any handle that may return `ZI_E_AGAIN` (would-block):

1. Attempt the operation (e.g. `zi_read`, `zi_write`, or a cap-specific request/response exchange).
2. If it returns `ZI_E_AGAIN`, ensure you have a `WATCH` installed for the appropriate readiness bit(s).
3. Call `POLL` and retry when a corresponding READY event arrives.

This keeps all blocking centralized in `sys/loop`.

### What “readable” means

`sys/loop` readiness is defined by the *runtime’s notion of progress* for a handle.

- For FD-backed byte streams (e.g. TCP), **readable** typically means the underlying fd is readable.
- For protocol handles implemented as a ZCL1 frame stream (e.g. `net/http`), **readable** can mean “a complete response/event frame is available to be read without blocking”.

In other words: a handle MAY be watchable even if it is not itself a raw socket, as long as the runtime can map it to a pollable source.

### Minimal example: wait for a server event frame

To wait for an async server event (such as `net/http` `EV_REQUEST`) without blocking inside the HTTP capability:

1. `WATCH` the HTTP capability handle for `readable`.
2. Call `POLL`.
3. On READY for that watch, read the next ZCL1 frame from the HTTP handle.

If the read still returns `ZI_E_AGAIN` (spurious wakeup / race), simply `POLL` again.

## Operations

All integers are little-endian.

### WATCH (op=1)

Register interest in readiness of a handle.

Request payload:
- `u32 handle`
- `u32 events` (bitset; see below)
- `u64 watch_id` (guest-chosen, MUST be non-zero and unique per open loop handle)
- `u32 flags` (MUST be `0` in v1)

`events` bits:
- `0x1` => readable
- `0x2` => writable
- `0x4` => hup (peer closed)
- `0x8` => error

OK response payload: empty.

Semantics:
- The watch remains active until `UNWATCH`.
- Watches are **level-triggered**: if the condition remains true, it MAY be reported again on subsequent `POLL` calls.
- If `handle` is not watchable by the runtime, the host MUST return a ZCL1 error.

### UNWATCH (op=2)

Remove a previously installed watch.

Request payload:
- `u64 watch_id`

OK response payload: empty.

Errors:
- If `watch_id` does not exist, the host MUST return a ZCL1 error.

### TIMER_ARM (op=3)

Arm a timer.

Request payload:
- `u64 timer_id` (guest-chosen, MUST be non-zero and unique per open loop handle)
- `u64 due_mono_ns`
- `u64 interval_ns` (0 = one-shot; non-zero = repeating)
- `u32 flags`

`flags` bits:
- `0x1` => `due_mono_ns` is relative (delay in nanoseconds from now)

OK response payload: empty.

Notes:
- Timers are driven by the host’s monotonic clock.
- If `interval_ns != 0`, the host SHOULD coalesce and/or catch up in a bounded way (implementation-defined), but MUST NOT emit unbounded bursts.

### TIMER_CANCEL (op=4)

Cancel a timer.

Request payload:
- `u64 timer_id`

OK response payload: empty.

Errors:
- If `timer_id` does not exist, the host MUST return a ZCL1 error.

### POLL (op=5)

Block until events are available, a timeout elapses, or the loop is interrupted.

Request payload:
- `u32 max_events` (MUST be >= 1)
- `u32 timeout_ms`

`timeout_ms` meanings:
- `0` => non-blocking poll
- `0xFFFFFFFF` => wait forever
- otherwise => wait up to that many milliseconds

OK response payload:
- `u32 version` (MUST be `1`)
- `u32 flags` (bitset; see below)
- `u32 event_count`
- `u32 reserved` (MUST be `0`)
- `event[event_count]` entries (fixed-size; see below)

`flags` bits:
- `0x1` => more events are pending (guest SHOULD call `POLL` again soon)

Each event entry is 32 bytes:
- `u32 kind`
- `u32 events` (bitset; meaning depends on kind)
- `u32 handle` (0 if not applicable)
- `u32 reserved` (MUST be `0`)
- `u64 id` (watch_id or timer_id)
- `u64 data` (kind-specific; v1 defines meanings below)

Event kinds:
- `1` => READY
  - `id` = `watch_id`
  - `handle` = watched handle
  - `events` = subset of requested readiness bits that became true
  - `data` = `0`
- `2` => TIMER
  - `id` = `timer_id`
  - `handle` = `0`
  - `events` = `0`
  - `data` = best-effort `monotonic_ns` at delivery time

Requirements:
- The host MUST return at most `max_events` entries.
- If no events are available by the timeout, the host MUST return `event_count = 0`.

## Backpressure and Fairness

- The host MUST bound internal buffering and MUST NOT allow unbounded event queue growth.
- The host SHOULD provide fairness across event sources (e.g. many sockets MUST NOT starve timers indefinitely).

## Errors

For malformed requests, unknown ops, policy violations, unknown IDs, or internal failures, implementations MUST return a ZCL1 error frame via `zi_zcl1_write_error(...)`.

## Implementation Notes (Non-Normative)

A high-performance implementation typically has:

- A **reactor backend**:
  - macOS: `kqueue`
  - Linux: `epoll`
  - (future) Windows: IOCP

- A **watch table** mapping `watch_id -> (handle, interest_mask)`.

- A per-handle integration mechanism so a capability can be “watchable” without exposing raw file descriptors to guests.
  Common approaches:
  1. Extend the internal handle table with an optional `poll` vtable (e.g. `get_fd` / `arm_read` / `arm_write`).
  2. Implement `sys/loop` inside the runtime core and let it directly recognize specific handle types (fast but more coupling).

- A **timer wheel / min-heap** for timers.

- (Optional but common) a **threadpool** for blocking tasks (e.g. filesystem, DNS), with completions delivered as loop events in a future protocol version.
