# zABI 2.5 readiness matrix (sys/loop@v1)

This document is a practical guide for guests using the **single blocking primitive** `sys/loop@v1`.

The model is:

- Any operation that would block returns `ZI_E_AGAIN`.
- The guest calls `sys/loop.WATCH` once per handle (and per interest set), then uses `sys/loop.POLL` to sleep.
- When `POLL` reports a READY event, retry the operation.
- Spurious wakeups are allowed: READY does not guarantee the next attempt will succeed.

## Readiness bits

`sys/loop@v1` readiness mask bits:

- `0x1` readable
- `0x2` writable
- `0x4` hup
- `0x8` error

Watches are **level-triggered**.

## What to WATCH for (by cap/operation)

Use this table when deciding which readiness bits to request in `WATCH` for a handle, based on which operation you plan to retry after `ZI_E_AGAIN`.

| Handle | Operation you’re retrying after `ZI_E_AGAIN` | WATCH events | Notes |
|---|---:|---:|---|
| `net/tcp` stream | `zi_read(stream, ...)` | `readable \| hup \| error` (`0x1 \| 0x4 \| 0x8`) | `hup/error` are optional but useful to wake on teardown; always retry `zi_read` after READY. |
| `net/tcp` stream | `zi_write(stream, ...)` | `writable \| hup \| error` (`0x2 \| 0x4 \| 0x8`) | Writable means “kernel send buffer / connect progress likely advanced”; always retry `zi_write`. |
| `net/tcp` stream (connect-in-progress) | first `zi_read`/`zi_write` returning `ZI_E_AGAIN` because connect not done | `writable \| hup \| error` (`0x2 \| 0x4 \| 0x8`) | Connect completion is detected on retry by the host; no separate “finish connect” op. |
| `net/tcp` listener | `zi_read(listener, ...)` (accept records) | `readable \| hup \| error` (`0x1 \| 0x4 \| 0x8`) | Readable means an `accept()` is likely to succeed; one read may return *batched* 32-byte accept records. |
| `file/aio` queue | `zi_read(aio, ...)` (completion frames) | `readable` (`0x1`) | Primary intended wait path: when `zi_read` yields `ZI_E_AGAIN`, `POLL` then retry. |
| `file/aio` queue | `zi_write(aio, ...)` (submit frames) returning `ZI_E_AGAIN` due to backpressure | `writable` (`0x2`) *(hint)* | Writability is a “submission queue has space” hint; still handle races (READY but next `zi_write` returns `ZI_E_AGAIN`). |

## Where `zi_ctl` fits

`zi_ctl(...)` is a synchronous syscall-style control plane (it is **not** a watchable handle), so there is nothing to `WATCH`.

Use `zi_ctl` to control other handles (e.g., handle operations like shutdown-write); then continue using normal readiness rules for those handles.
