# Future

- Futures library (guest-side SIR).

Current MVP API (v0):
- `future_sleep_ms(ms:i32) -> ptr`
- `future_block_on_i32(fut:ptr, out_ptr:ptr) -> i32`
- `future_free(fut:ptr) -> i32`

Value / poll contract:
- This MVP supports *i32-valued* futures.
- `future_block_on_i32` drives `fut` until it returns:
	- `0` = pending
	- `1` = ready (the future wrote an `i32` to `out_ptr`)
	- `<0` = error

Representation (current implementation):
- Futures are heap-allocated records with a `kind` + kind-specific state (no function pointers, no ptr/int casts).
- Dispatch is done by `kind` inside `future_poll_i32`.

Ownership:
- `future_sleep_ms` allocates the future object; caller should free it with `future_free` once no longer needed.
- `future_block_on_i32` does not currently free `fut` for you.

Executor:
- `future_block_on_i32` opens `sys:loop@v1` and drives the future by:
	- polling it
	- calling `sys/loop.POLL` when it returns pending
	- providing the last POLL response via the waker context so futures can detect timer/readiness events without blocking.

Error codes:
- Negative return values are executor/future errors. Current meanings are defined in `future.sir` (e.g. open-loop failed, deadlock, poll failed, timer arm failed).

Notes:
- This is intentionally minimal so we can iterate quickly; when we add I/O futures we may extend the waker with WATCH/UNWATCH helpers.