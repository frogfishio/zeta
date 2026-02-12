# Files

- Guest-side file and directory helpers built on the golden capability `file:aio@v1`.

This module targets the real zABI surface:
- Open the cap via `zi_cap_open` with kind="file", name="aio".
- Send requests as ZCL1 frames over the returned handle.
- Read immediate ACKs and asynchronous `EV_DONE` completion frames.

For good DX, [file/file.sir](src/guestlib/file/file.sir) provides synchronous helpers like:
- `file_read_all_alloc_*` (read a whole file into a fresh buffer)
- `file_write_all_*` (create/truncate and write a whole file)
- `file_stat_*`, `file_mkdir_*`, `file_unlink_*`

Most helpers use `sys:loop` (via `loop_wait_readable_until`) to wait for completions.

## Notes on paths / strings

Prefer the `(path, path_len)` APIs.

The `*_cstr` helpers call `file_cstr_len()` (scan for `\0`). Under `sem`, SIR string literals are not guaranteed to be NUL-terminated, so `*_cstr` can trap with `ZI_E_BOUNDS`. Use `*_cstr` only if you know the pointer refers to a NUL-terminated string in guest memory.

## Stat helpers

`STAT` returns a fixed 32-byte little-endian blob. Use:
- `file_stat_size`, `file_stat_mtime_ns`, `file_stat_mode`, `file_stat_uid`, `file_stat_gid`