# Standard guest side library

- `zcl1.sir`: ZCL1 framing helpers (24-byte header) for zABI 2.5 protocols
- `zabi.sir`: small wrappers over zABI externs (starting with `zi_ctl CAPS_LIST`)

Next additions (planned):

- stdout: `puts`, `println`, small formatting helpers
- log: telemetry helpers
- ctl: higher-level `zi_ctl` helpers and parsing
- loop/file/tcp: guest-side helpers for golden caps (`sys/loop`, `file/aio`, `net/tcp`)