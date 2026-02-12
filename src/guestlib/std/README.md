# Standard guest side library

- `zcl1.sir`: ZCL1 framing helpers (24-byte header) for zABI 2.5 protocols
- `zabi.sir`: small wrappers over zABI externs (starting with `zi_ctl CAPS_LIST`)
- `stdout.sir`: stdout/stderr helpers (`stdout_puts`, `stdout_println`, small formatting)
- `stdin.sir`: stdin helpers (`stdin_read`, small helpers)
- `log.sir`: telemetry helpers (wraps `zi_telemetry`)
- `ctl.sir`: higher-level `zi_ctl` helpers and error parsing

Next additions (planned):
- file/tcp: guest-side helpers for golden caps (`file/aio`, `net/tcp`)

Implemented outside `std/`:
- `../loop/loop.sir`: guest-side helpers for the golden cap `sys:loop`