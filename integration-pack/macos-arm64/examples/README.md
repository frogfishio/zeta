# Examples

These examples are designed for **integrators**:

- `host_shim/` — minimal runner that installs zABI 2.5 host hooks (`zi_hostlib25_init_all`) and calls a lowered entrypoint (`zir_main`).
- `sys_info_time_now/` — native host example that opens `sys/info@v1` and requests `TIME_NOW` over ZCL1.
