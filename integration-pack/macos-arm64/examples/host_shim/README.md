# host_shim (native runner for `lower` outputs)

This folder is a minimal template for running a `lower`-produced object file against the zABI 2.5 host runtime.

It demonstrates:

- Installing host hooks + registering “golden” capabilities via `zi_hostlib25_init_all()`
- Calling a lowered entrypoint (`zir_main`) from a normal C `main()`

## Quick start (included echo program)

```sh
./build.sh ./echo_zabi25_native.jsonl
printf 'hello\n' | ./guest
```

## Notes

- Your IR must `PUBLIC` an entry symbol (this template expects `zir_main`).
- Your IR must `EXTERN` any `zi_*` syscalls it calls (e.g. `zi_read`, `zi_write`).
