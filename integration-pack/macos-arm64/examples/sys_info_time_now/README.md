# sys/info TIME_NOW (host example)

This example shows how an embedding (your native host program) can query the zABI 2.5 capability `sys/info@v1` to obtain time values.

It demonstrates:

- Initializing zingcore 2.5 host wiring via `zi_hostlib25_init_all()`
- Opening a capability handle via `zi_cap_open()`
- Sending a ZCL1 request frame via `zi_write()`
- Reading/parsing the ZCL1 response via `zi_read()` + `zi_zcl1_parse()`

## Build + run

From the platform pack root (e.g. `dist/integration-pack/macos-arm64/`):

```sh
cd examples/sys_info_time_now
./build.sh
./sys_info_time_now
```

Expected output is something like:

- `sys/info@v1 TIME_NOW realtime_ns=... monotonic_ns=...`
