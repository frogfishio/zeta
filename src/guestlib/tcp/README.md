# TCP

- Guest-side helpers for the golden cap `net:tcp@v1`.

This module is intentionally low-level and nonblocking so you can build:

- nginx-style servers (many sockets, readiness-driven)
- reverse-proxy / HTTP clients (connect + write request + read response)

## What you get

- Open helpers:
	- `tcp_connect` / `tcp_connect_cstr`
	- `tcp_listen` / `tcp_listen_cstr` (supports `port=0` + `out_port_ptr`)
- Listener accept parsing:
	- `tcp_accept_one` + `tcp_accept_parse`
- Read/write helpers that wait via `sys:loop`:
	- `tcp_write_all_until`
	- `tcp_read_exact_until`
- Half-close support:
	- `tcp_shutdown_write` (maps to `ZI_HANDLE_OP_SHUT_WR` via `zi_ctl`)

## Notes

- All I/O is nonblocking; would-block returns `ZI_E_AGAIN`.
- The helpers here use `loop_wait_*_until(...)` convenience wrappers.
	For a real nginx-like server, you’ll typically keep one loop handle open and
	multiplex many sockets with `WATCH`/`POLL`.