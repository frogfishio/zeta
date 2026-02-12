# LOOP cap guest-side handler

- Essential for async/futures and for caps like `file:aio` and `net:tcp`.

Implemented in `loop.sir`:

- `loop_open()` opens the golden `sys:loop@v1` capability.
- `loop_timer_arm(...)` and `loop_poll(...)` speak the ZCL1-framed sys/loop protocol over the returned handle.
- `loop_sleep_ms(ms)` is an MVP helper implemented via a relative timer + `POLL`.
- `loop_watch(...)` / `loop_unwatch(...)` frame WATCH operations.
- `loop_wait_readable(handle, timeout_ms)` / `loop_wait_writable(handle, timeout_ms)` are convenience helpers built on WATCH+POLL.
- `loop_wait_readable_until(handle, timeout_ms)` / `loop_wait_writable_until(handle, timeout_ms)` repeatedly POLL in bounded slices until ready or timeout.
