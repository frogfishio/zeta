# file/aio v1 Protocol Specification

**Capability Name:** `file/aio`  
**Version:** 1  
**Kind:** `"file"`  
**Name:** `"aio"`

## Overview

`file/aio@v1` provides completion-based filesystem I/O that fits the single-wait model:

- Operations are submitted as ZCL1 request frames written to the `file/aio` handle.
- Submission is acknowledged immediately with an OK or ERROR response frame.
- Completion is delivered asynchronously as a ZCL1 frame with `op = EV_DONE` and `rid` equal to the original request `rid`.
- The `file/aio` handle is pollable for readability via `sys/loop`.

Sandboxing:

- If `ZI_FS_ROOT` is set, guest paths must be absolute and are resolved under that root.
- `..` traversal is rejected.
- Symlinks are rejected in any path segment.

## Handle lifecycle

Open:

- `zi_cap_open(kind="file", name="aio", params_len=0)` → returns a stream handle.

Close:

- `zi_end(handle)` closes the queue and releases all host resources.

## ZCL1 framing

Requests and responses are ZCL1 frames (`zi_zcl1.h`).

- Request `rid` is the **job id** chosen by the guest.
- Immediate response uses the same `(op, rid)` as the request.

Completion frames:

- `op = EV_DONE (100)`
- `rid = original request rid`

## Operations

All integers are little-endian.

### OPEN (op=1)

Request payload (20 bytes):

- `u64 path_ptr` (UTF-8 bytes, not NUL-terminated)
- `u32 path_len`
- `u32 oflags` (`ZI_FILE_O_*`)
- `u32 create_mode` (POSIX mode bits; used if `ZI_FILE_O_CREATE` is set)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`, rid=job id):

OK payload:

- `u16 orig_op = 1`
- `u16 reserved = 0`
- `u32 result = 0`
- `u64 file_id`

### CLOSE (op=2)

Request payload (8 bytes):

- `u64 file_id`

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 2`
- `u16 reserved = 0`
- `u32 result = 0`

### READ (op=3)

Request payload (24 bytes):

- `u64 file_id`
- `u64 offset`
- `u32 max_len`
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 3`
- `u16 reserved = 0`
- `u32 result = nbytes`
- `bytes[nbytes]` (inline)

### WRITE (op=4)

Request payload (32 bytes):

- `u64 file_id`
- `u64 offset`
- `u64 src_ptr`
- `u32 src_len`
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 4`
- `u16 reserved = 0`
- `u32 result = nbytes_written`

### MKDIR (op=5)

Request payload (20 bytes):

- `u64 path_ptr`
- `u32 path_len`
- `u32 mode` (POSIX mode bits; if 0, runtime may choose a default such as `0755`)
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 5`
- `u16 reserved = 0`
- `u32 result = 0`

### RMDIR (op=6)

Request payload (16 bytes):

- `u64 path_ptr`
- `u32 path_len`
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 6`
- `u16 reserved = 0`
- `u32 result = 0`

### UNLINK (op=7)

Request payload (16 bytes):

- `u64 path_ptr`
- `u32 path_len`
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 7`
- `u16 reserved = 0`
- `u32 result = 0`

### STAT (op=8)

Request payload (16 bytes):

- `u64 path_ptr`
- `u32 path_len`
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 8`
- `u16 reserved = 0`
- `u32 result = 0`
- extra (32 bytes):
	- `u64 size`
	- `u64 mtime_ns`
	- `u32 mode`
	- `u32 uid`
	- `u32 gid`
	- `u32 reserved = 0`

### READDIR (op=9)

Request payload (20 bytes):

- `u64 path_ptr`
- `u32 path_len`
- `u32 max_bytes` (maximum extra bytes in completion; runtime clamps)
- `u32 flags` (must be 0)

Immediate response:

- OK with empty payload, or ERROR.

Completion (`EV_DONE`):

OK payload:

- `u16 orig_op = 9`
- `u16 reserved = 0`
- `u32 result = entry_count`
- extra:
	- `u32 flags` (`bit0 = truncated`)
	- repeated `entry_count` times:
		- `u32 dtype` (`0=unknown, 1=file, 2=dir, 3=symlink, 4=other`)
		- `u32 name_len`
		- `bytes[name_len]` (UTF-8 bytes, not NUL-terminated)

## Error handling

- Submission errors (bad payload, out-of-bounds pointers, queue full) are returned as an immediate ERROR response.
- Execution errors (open/read/write failures, unknown `file_id`, sandbox denial) are returned as an `EV_DONE` ERROR frame.

## Backpressure (guest-level contract)

`file/aio` has a bounded internal submission queue. When the queue is full, the host MUST reject additional job submissions.

### What the guest observes

- The job submission itself is still a normal ZCL1 request written to the `file/aio` handle.
- If the queue is full, the host MUST return an **immediate** ZCL1 ERROR response for that `(op, rid)`.

In the zingcore 2.5 reference implementation, this error uses:

- `trace_len/trace` = `"file.aio"`
- `msg_len/msg` = `"queue full"`

These strings are not a general ZCL1 standard, but they are the stable shape used by zingcore 2.5 today.
Guests SHOULD treat this condition as a retryable backpressure signal.

### How to wait (no “queue has space” readiness)

The intended guest strategy is:

- Guests SHOULD keep a bounded number of in-flight jobs.
- On a `queue full` submission error, guests SHOULD stop submitting new jobs for that queue.
- Guests SHOULD drain completions/events from the same `file/aio` handle.
- Guests SHOULD use `sys/loop` to block until the queue becomes **readable** (meaning: there are frames to read), then read and process them.
- After observing progress (typically one or more `EV_DONE` frames), guests SHOULD retry the submission.

This avoids busy-looping while still fitting the single-wait model.

### Submission writable readiness (queue has space)

Guests MAY also watch the `file/aio` handle for `writable` readiness to wait for submission space.

Contract:

- When the submission queue is full, the host SHOULD report the handle as not-writable.
- When the queue transitions from full to having at least one free slot, the host SHOULD report the handle as writable.

This is a readiness hint and is still subject to races:

- A guest might observe `writable` and still lose a race and get `queue full`.
- Guests MUST handle `queue full` as the authoritative signal.

Intended guest pattern:

- On `queue full`, WATCH `writable` and `POLL`.
- Retry submission on READY(writable).

Pseudocode sketch:

```c
for (;;) {
	submit_job(aio, rid, ...);
	frame ack = read_frame_wait(loop, aio, watch_id);

	if (ack.status == ERROR && ack.trace == "file.aio" && ack.msg == "queue full") {
		// Backpressure: wait for progress (completions) before retrying.
		do {
			frame f = read_frame_wait(loop, aio, watch_id);
			// process f (ACK/EV_DONE/ERROR)
		} while (!saw_ev_done_for_any_job());
		continue; // retry submit
	}

	break;
}
```

## Notes

- This version returns READ data inline in completion frames. Large reads may be truncated by the runtime.
- Guests should WATCH the queue handle for readability and use `sys/loop.POLL` to await completions without busy-waiting.

## Guest-side parsing recipes

This section is non-normative, but intended to be copy/paste friendly.

### Waiting for completions via sys/loop (no busy-wait)

The `file/aio` queue handle is watchable for **readable** readiness. The intended blocking pattern is:

1. Open `sys/loop@v1`.
2. `WATCH` the `file/aio` handle for `readable`.
3. Whenever `zi_read(file_aio, ...)` returns `ZI_E_AGAIN`, call `sys/loop.POLL` and retry.

Notes:

- A READY event can be **spurious** (race); always retry the read after waking.
- The watch is level-triggered; if completions are already buffered, POLL may return immediately.

Pseudocode (C-ish):

```c
// Open sys/loop (params empty)
zi_handle_t loop = cap_open("sys", "loop", NULL, 0);

// WATCH file/aio for readable
uint64_t watch_id = 1;
sys_loop_watch(loop, /*handle=*/aio, /*events=*/0x1 /* readable */, watch_id);

int read_frame_wait(uint8_t *out, uint32_t cap) {
	uint32_t off = 0;
	for (;;) {
		int32_t n = zi_read(aio, out + off, cap - off);
		if (n == ZI_E_AGAIN) {
			// Block until the queue becomes readable (or timeout).
			// timeout_ms = 0xFFFFFFFF means wait forever.
			sys_loop_poll(loop, /*max_events=*/16, /*timeout_ms=*/0xFFFFFFFF);
			continue;
		}
		if (n <= 0) return 0;
		off += (uint32_t)n;

		// ZCL1 frame is complete once we have header (24) + payload_len.
		if (off >= 24) {
			uint32_t payload_len = read_u32le(out + 20);
			if (off >= 24u + payload_len) return (int)(24u + payload_len);
		}
	}
}
```

### READDIR completion parsing

On success, the completion is an OK frame with:

- `op = EV_DONE (100)`
- `rid = original job id`
- payload starts with `orig_op/reserved/result` (8 bytes)

For `orig_op = READDIR (9)`:

- `result = entry_count`
- extra begins with `u32 flags` then `entry_count` packed entries.

Each packed entry is:

- `u32 dtype`
- `u32 name_len`
- `bytes[name_len]`

Pseudocode (C-ish, assumes `pl` points at the EV_DONE payload, and `pl_len` is its length):

```c
uint16_t orig_op = read_u16le(pl + 0);
uint32_t result  = read_u32le(pl + 4);

if (orig_op == 9 /* READDIR */) {
	uint32_t entry_count = result;
	if (pl_len < 8 + 4) fail();

	const uint8_t *p = pl + 8;
	uint32_t left = pl_len - 8;

	uint32_t flags = read_u32le(p);
	p += 4; left -= 4;

	for (uint32_t i = 0; i < entry_count; i++) {
		if (left < 8) fail();
		uint32_t dtype = read_u32le(p + 0);
		uint32_t nlen  = read_u32le(p + 4);
		p += 8; left -= 8;
		if (left < nlen) fail();

		// name bytes are not NUL-terminated
		const uint8_t *name = p;
		// ... consume ...
		p += nlen; left -= nlen;
	}

	if (flags & 1u) {
		// truncated: retry with a larger max_bytes if you need more entries
	}
}
```

### STAT completion parsing

For `orig_op = STAT (8)`, the OK payload is:

- header (8 bytes): `orig_op/reserved/result`
- extra (32 bytes): `size, mtime_ns, mode, uid, gid, reserved`

Pseudocode:

```c
if (orig_op == 8 /* STAT */) {
	if (pl_len != 8 + 32) fail();
	uint64_t size     = read_u64le(pl + 8);
	uint64_t mtime_ns = read_u64le(pl + 16);
	uint32_t mode     = read_u32le(pl + 24);
	uint32_t uid      = read_u32le(pl + 28);
	uint32_t gid      = read_u32le(pl + 32);
}
```
